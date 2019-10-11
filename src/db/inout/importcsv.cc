/*
 *
 * Copyright (C) 2015 - 2018 Eaton
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*! \file  importcsv.cc
    \brief Implementation of csv import
    \author Michal Vyskocil <MichalVyskocil@Eaton.com>
    \author Alena Chernikava <AlenaChernikava@Eaton.com>
*/

#include <string>
#include <algorithm>
#include <ctype.h>
#include <unordered_set>
#include <limits>
#include <cstddef>

#include <tntdb/connect.h>
#include <cxxtools/regex.h>
#include <cxxtools/join.h>

#include <fty_proto.h>
#include <fty_common_rest.h>
#include <fty_common_db_dbpath.h>
#include <fty_common_db.h>
#include <fty_common_mlm_tntmlm.h>
#include <fty_common_mlm_sync_client.h>
#include <fty_asset_activator.h>

#include "../../persist/assetcrud.h"
#include "cleanup.h"
#include "../asset_general.h"
#include "db/dbhelpers.h"
#include "../inout.h"
#include "shared/utils.h"
#include "shared/utils_json.h"
#include "shared/utilspp.h"

#define AGENT_ASSET_ACTIVATOR "etn-licensing-credits"

using namespace shared;

namespace persist {

typedef struct _LIMITATIONS_STRUCT
{
    int max_active_power_devices;
    int global_configurability;
} LIMITATIONS_STRUCT;

int
    get_priority
        (const std::string& s)
{
    if ( s.size() > 2 )
        return 5;
    for (int i = 0; i != 2; i++) {
        if (s[i] >= 49 && s[i] <= 53) {
            return s[i] - 48;
        }
    }
    return 5;
}

static std::map<std::string,int>
    read_element_types
        (tntdb::Connection &conn)
{
    auto res = get_dictionary_element_type(conn);
    // in case of any error, it would be empty
    return res.item;
}

static std::map<std::string,int>
    read_device_types
        (tntdb::Connection &conn)
{
    auto res = get_dictionary_device_type(conn);
    // in case of any error, it would be empty
    return res.item;
}

static bool
    check_u_size
        (std::string &s)
{
    static cxxtools::Regex regex("^[0-9]{1,2}[uU]?$");
    if ( !regex.match(s) )
    {
        return false;
    }
    else
    {
        // need to drop the "U"
        if ( ! (::isdigit(s.back())) )
        {
            s.pop_back();
        }
        // remove trailing 0
        if (s.size() == 2 && s[0] == '0') {
            s.erase(s.begin());
        }
        return true;
    }
}

static bool
    match_ext_attr
        (std::string &value, const std::string &key)
{
    if ( key == "u_size" )
    {
        return check_u_size(value);
    }
    return ( !value.empty() );
}

// check if key contains date
// ... warranty_end or ends with _date
static bool
    is_date (const std::string &key)
{
    static cxxtools::Regex regex{"(^warranty_end$|_date$)"};
    return regex.match (key);
}

static double
sanitize_value_double(
    const std::string &key,
    const std::string &value)
{
    try {
        std::size_t pos = 0;
        double d_value = std::stod (value, &pos);
        if  ( pos != value.length() ) {
            log_info ("Extattribute: %s='%s' is not double", key.c_str(), value.c_str());
            bios_throw ("request-param-bad", key.c_str(), ("'" + value + "'").c_str(), "value should be a number");
        }
        return d_value;
    }
    catch (const std::exception &e ) {
        log_info ("Extattribute: %s='%s' is not double", key.c_str(), value.c_str());
        bios_throw ("request-param-bad", key.c_str(), ("'" + value + "'").c_str(), "value should be a number");
    }
}


/*
 * \brief Case insensitive comparison
 */
bool findStringIC(const std::string &strHaystack, const std::string &strNeedle)
{
    auto it = std::search (strHaystack.begin(), strHaystack.end(), strNeedle.begin(), strNeedle.end(),
            [](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); } );
  return (it != strHaystack.end() );
}


/*
 * \brief Verify if column named column_name contains specified to_check value.
 * \return 0 on failure, all values in such column are empty (so other columns may match)
 * \return -1 on failure, but column has valid content (therefore match is impossible)
 * \return >0 on successfull match
 */
int check_column_match(
        const std::string &column_name,
        std::unordered_set<std::string> &to_check,
        std::set<std::string> &unused_columns,
        std::vector<size_t> &rcs_in_csv,
        const CsvMap &cm,
        bool match_mode,
        bool fail_on_not_found = true) {
    if (to_check.empty()) {
        return 0;
    }
    if (unused_columns.count(column_name)) {
        char check = 0;
        if (!to_check.empty()) {
            for (size_t row_i : rcs_in_csv) {
                std::string verify = cm.get(row_i, column_name);
                if (match_mode) {
                    //optimized for find
                    auto found = to_check.find(verify);
                    if (found != to_check.end()) {
                        log_debug("Picked by %s matching '%s'='%s'", column_name.c_str(), verify.c_str(), found->c_str());
                        return row_i;
                    }
                } else {
                    //less optimal for iteration
                    for (auto to_check_element : to_check) {
                        if (findStringIC(verify, to_check_element)) {
                            log_debug("Picked by %s partial match", column_name.c_str());
                            return row_i;
                        }
                    }
                }
                if (!check && !verify.empty()) {
                    check = 1;
                }
            }
        }
        if (check && fail_on_not_found) {
            log_debug("Failed to pick by %s", column_name.c_str());
            return -1;
        }
    }
    return 0;
}


/*
 * \brief Overloading of check_column_match for easier usage
 */
int check_column_match(
        const std::string &column_name_str,
        char *to_check,
        std::set<std::string> &unused_columns,
        std::vector<size_t> &rcs_in_csv,
        const CsvMap &cm,
        bool match_mode = true,
        bool fail_on_not_found = true) {
    std::unordered_set<std::string> to_check_set;
    if (NULL != to_check) {
        to_check_set.emplace(to_check);
    }
    return check_column_match(column_name_str, to_check_set,
            unused_columns, rcs_in_csv, cm, match_mode, fail_on_not_found);
}


/*
 * \brief Identify id of row with rackcontroller-0
 * \return size_t::max when disabled
 * \return int number of RC-0 row
 * \return number of rows + 1 when RC-0 not found
 * \throw runtime_error on failure
 */
int
promote_rc0(
        MlmClient *client,
         const CsvMap& cm,
         touch_cb_t touch_fn) {
    char have_ids = 0;
    int rc0_row = -1;
    char *info_ip1 = NULL;
    char *info_ip2 = NULL;
    char *info_ip3 = NULL;
    char *info_serial = NULL;
    char *info_hostname = NULL;
    zhash_t *myself_db_ext = NULL;
    std::vector<char *> rcs_in_db;
    std::vector<size_t> rcs_in_csv;

    log_debug("Function called: promote_rc0");
    touch_fn(); // renew request watchdog timer
    // first find all rows with rcs_in_db
    auto unused_columns = cm.getTitles();
    if (unused_columns.empty()) {
        std::string err = TRANSLATE_ME ("Cannot import empty document.");
        bios_throw("bad-request-document", err.c_str ());
    }
    have_ids = unused_columns.count("id");
    for (size_t row_i = 1; row_i != cm.rows(); row_i++) {
        std::string type = unused_columns.count("type") ? cm.get(row_i, "type") : "notype";
        std::string subtype = unused_columns.count("sub_type") ? cm.get(row_i, "sub_type") : "nosubtype";
        if (0 == type.compare(0, strlen("device"), "device") && 0 == subtype.compare(0, strlen("rackcontroller"), "rackcontroller")) {
            log_debug("CSV contains RC on row %u", row_i);
            rcs_in_csv.push_back(row_i);
            if (have_ids && 0 == cm.get(row_i, "id").compare("rackcontroller-0")) {
                rc0_row = rcs_in_csv.size() - 1;
                log_debug("identified RC-0 as %d item of RCs list", row_i);
            }
        }
    }
    if (0 == rcs_in_csv.size()) {
        log_debug("There are no RCs in CSV, returning -1");
        return cm.rows() + 1;
    }
    touch_fn(); // renew request watchdog timer

    // ask for data about myself - info first
    zuuid_t *uuid = zuuid_new ();
    zmsg_t *msg = zmsg_new ();
    zmsg_addstr (msg, "INFO");
    zmsg_addstr (msg, zuuid_str_canonical (uuid));
    if (-1 == client->sendto ("fty-info", "info", 1000, &msg)) {
        zuuid_destroy (&uuid);
        zmsg_destroy (&msg);
        throw std::runtime_error(TRANSLATE_ME ("Sending INFO message to fty-info failed"));
    }
    touch_fn(); // renew request watchdog timer
    // parse receieved data
    zmsg_t *resp = client->recv (zuuid_str_canonical (uuid), 5);
    touch_fn(); // renew request watchdog timer
    if (NULL == resp) {
        zuuid_destroy (&uuid);
        throw std::runtime_error(TRANSLATE_ME ("Response on INFO message from fty-info is empty"));
    }
    char *command = zmsg_popstr (resp);
    if (0 == strcmp("ERROR", command)) {
        zuuid_destroy (&uuid);
        zstr_free (&command);
        zmsg_destroy (&resp);
        throw std::runtime_error(TRANSLATE_ME ("Response on INFO message from fty-info is ERROR"));
    }
    char *srv_name  = zmsg_popstr (resp); // we don't really need those, but we need to popstr them
    char *srv_type  = zmsg_popstr (resp);
    char *srv_stype = zmsg_popstr (resp);
    char *srv_port  = zmsg_popstr (resp);
    log_debug("Receieved message from info: %s %s %s %s %s", command, srv_name, srv_type, srv_stype, srv_port);
    zframe_t *frame_infos = zmsg_next (resp);
    if (NULL == frame_infos) {
        zuuid_destroy (&uuid);
        zstr_free (&command);
        zstr_free(&srv_name);
        zstr_free(&srv_type);
        zstr_free(&srv_stype);
        zstr_free(&srv_port);
        zframe_destroy (&frame_infos);
        zmsg_destroy (&resp);
        throw std::runtime_error(TRANSLATE_ME ("Response on INFO message from fty-info miss zhash frame"));
    }
    zhash_t *info = zhash_unpack(frame_infos); // serial, hostname, ip.[1-3]
    /*char * item = (char *)zhash_first(info);
    while (NULL != item) {
        item = (char *)zhash_next(info);
    }*/
    zstr_free (&command);
    zstr_free(&srv_name);
    zstr_free(&srv_type);
    zstr_free(&srv_stype);
    zstr_free(&srv_port);
    info_ip1 = (char *)zhash_lookup(info, "ip.1");
    info_ip2 = (char *)zhash_lookup(info, "ip.2");
    info_ip3 = (char *)zhash_lookup(info, "ip.3");
    info_serial = (char *)zhash_lookup(info, "serial");
    info_hostname = (char *)zhash_lookup(info, "hostname");
    log_debug("Receieved message from info: ip1=%s, ip2=%s, ip3=%s, serial=%s, hostname=%s",
            info_ip1, info_ip2, info_ip3, info_serial, info_hostname);
    zmsg_destroy (&resp);
    touch_fn(); // renew request watchdog timer

    // ask for data about myself - asset next
    msg = zmsg_new ();
    zmsg_addstr (msg, "GET");
    //FIXME: use new UUID here
    zmsg_addstr (msg, zuuid_str_canonical (uuid));
    zmsg_addstr (msg, "rackcontroller-0");
    if (-1 == client->sendto ("asset-agent", "ASSET_DETAIL", 1000, &msg)) {
        zuuid_destroy (&uuid);
        zmsg_destroy (&msg);
        throw std::runtime_error(TRANSLATE_ME ("Sending ASSET_DETAIL message to fty-asset failed"));
    }
    touch_fn(); // renew request watchdog timer
    // parse receieved data
    resp = client->recv (zuuid_str_canonical (uuid), 5);
    touch_fn(); // renew request watchdog timer
    if (NULL == resp) {
        zuuid_destroy (&uuid);
        throw std::runtime_error(TRANSLATE_ME ("Response on ASSET_DETAIL message from fty-asset is empty"));
    }
    fty_proto_t *myself_db = NULL;
    if (fty_proto_is (resp)) {
        myself_db = fty_proto_decode (&resp);
        if (NULL == myself_db) {
            myself_db_ext = NULL;
        } else {
            myself_db_ext = fty_proto_get_ext(myself_db);
        }
        // workaround for having count of RC assets, as we consider only 0/1+ state
        rcs_in_db.push_back((char*)"rackcontroller-0");
    } else {
        log_debug("Receieved message that is not fty_proto");
        zmsg_destroy (&resp);
        myself_db_ext = NULL;
    }
    if (NULL == myself_db_ext) {
        log_debug("Receieved message from asset: no data about RC-0");
        myself_db_ext = zhash_new();
    } else {
        log_debug("Receieved message from asset: RC-0 data gained successfully");
    }
    fty_proto_destroy (&myself_db);
    touch_fn(); // renew request watchdog timer

    /*
     * this code was disabled due to performance issues, and assuming that this code followed
     * various updates to DB and other agents, we always have RC-0 when there is at least one
     * RC in our DB. Should this code be reactivated again, remote the workaround above.
    // get list of rcs_in_db already in database
    msg = zmsg_new ();
    zmsg_addstr (msg, "GET");
    zmsg_addstr (msg, zuuid_str_canonical (uuid));
    zmsg_addstr (msg, "rackcontroller");
    int rv = client->sendto ("asset-agent", "ASSETS", 5000, &msg);
    touch_fn(); // renew request watchdog timer
    if (rv != 0) {
        zhash_destroy(&info);
        throw std::runtime_error("Request for rackcontroller list failed");
    }
    touch_fn(); // renew request watchdog timer
    resp = client->recv (zuuid_str_canonical (uuid), 5);
    touch_fn(); // renew request watchdog timer
    command = zmsg_popstr (resp);
    if (0 == strcmp("ERROR", command)) {
        zuuid_destroy (&uuid);
        zhash_destroy(&info);
        throw std::runtime_error("Response on GET message from fty-asset is ERROR");
    }
    zstr_free (&command);
    char *asset = zmsg_popstr(resp);
    while (asset) {
        rcs_in_db.push_back(asset);
        log_debug("From database came rackcontroller: %s", asset);
        asset = zmsg_popstr(resp);
    }
    zmsg_destroy (&resp);
    zuuid_destroy (&uuid);
    touch_fn(); // renew request watchdog timer
    */

    size_t retval = cm.rows() + 1;
    do { // easier memory freeing
        // check how many RCs we already have in database
        if (0 == rcs_in_db.size()) {
            log_debug("0 RCs in database");
            // promote the only one RC to RC-0
            if (1 == rcs_in_csv.size()) {
                log_debug("Get the only one");
                retval = rcs_in_csv[0];
                break;
            }
            // multiple RCs, check for RC-0 for promotion
            if (-1 != rc0_row) {
                log_debug("Pick one marked as RC-0");
                retval = rcs_in_csv[rc0_row];
                break;
            }
        } else {
            log_debug("1+ RCs in database");
        }
        // check for special case = 1 RC in DB, 1 RC in CSV, internal id not set
        if (1 == rcs_in_db.size() && 1 == rcs_in_csv.size() && 0 == unused_columns.count("id")) {
            retval = rcs_in_csv[0];
            log_debug("Picked one by special condition 1RC in CSV and DB, id column not set");
            break;
        }
        // check for uuid match
        char *my_uuid = (char *)zhash_lookup (myself_db_ext, "uuid");
        retval = check_column_match("uuid", my_uuid, unused_columns, rcs_in_csv, cm);
        if (0 != retval) break;
        // check for serial number match
        char *checked_sn = (char *)zhash_lookup (myself_db_ext, "serial_no");
        if (NULL == checked_sn || 0 == strcmp(checked_sn, "")) {
            checked_sn = info_serial;
        }
        retval = check_column_match("serial_no", checked_sn, unused_columns, rcs_in_csv, cm);
        if (0 != retval) break;
        // check hostname match
        char *checked_hname = (char *)zhash_lookup (myself_db_ext, "hostname.1");
        if (NULL == checked_hname || 0 == strcmp(checked_hname, "")) {
            checked_hname = info_hostname;
        }
        retval = check_column_match("hostname.1", checked_hname, unused_columns, rcs_in_csv, cm);
        if (0 != retval) break;
        // check for fqdn match
        char *my_fqdn = (char *)zhash_lookup (myself_db_ext, "fqdn.1");
        retval = check_column_match("fqdn.1", my_fqdn, unused_columns, rcs_in_csv, cm);
        if (0 != retval) break;
        // check name contains hostname
        retval = check_column_match("name", checked_hname, unused_columns, rcs_in_csv, cm, false, false);
        if (0 != retval) break;
        // check name contains fqdn
        retval = check_column_match("name", my_fqdn, unused_columns, rcs_in_csv, cm, false, false);
        if (0 != retval) break;
        // then check ip address
        char *my_ip1 = (char *)zhash_lookup (myself_db_ext, "ip.1");
        if (NULL != my_ip1 && 0 != strcmp(my_ip1, "") &&
                0 != strcmp(my_ip1, "127.0.0.1") && 0 != strcmp(my_ip1, "::1")) {
            retval = check_column_match("ip.1", my_ip1, unused_columns, rcs_in_csv, cm);
            if (0 != retval) break;
        } else {
            std::unordered_set<std::string> ip_set;
            if (NULL != info_ip1 && 0 != strcmp(info_ip1, "") &&
                    0 != strcmp(info_ip1, "127.0.0.1") && 0 != strcmp(info_ip1, "::1")) {
                ip_set.emplace(info_ip1);
            }
            if (NULL != info_ip2 && 0 != strcmp(info_ip2, "") &&
                    0 != strcmp(info_ip2, "127.0.0.1") && 0 != strcmp(info_ip2, "::1")) {
                ip_set.emplace(info_ip2);
            }
            if (NULL != info_ip3 && 0 != strcmp(info_ip3, "") &&
                    0 != strcmp(info_ip3, "127.0.0.1") && 0 != strcmp(info_ip3, "::1")) {
                ip_set.emplace(info_ip3);
            }
            if (!ip_set.empty()) {
                retval = check_column_match("ip.1", info_ip1, unused_columns, rcs_in_csv, cm);
                if (0 != retval) break;
            }
        }
        // database is empty, or this is considered initial import, take first available
        if (0 == rcs_in_db.size() || 0 == unused_columns.count("id")) {
            if (-1 != rc0_row) {
                log_debug("Pick one marked as RC-0");
                retval = rcs_in_csv[rc0_row];
                break;
            }
            log_debug("Picked first available");
            retval = rcs_in_csv[0];
            break;
        }
        retval = cm.rows () + 1;
    } while (0);
    zhash_destroy(&info);
    if (unused_columns.count("name") && retval < cm.rows()) {
        log_debug("Resulting RC-0 output is %zu, matching RC named '%s'", retval, cm.get(retval, "name").c_str());
    } else {
        retval = cm.rows() + 1;
        log_debug("Resulting RC-0 output is %zu, having no name", retval);
    }
    touch_fn(); // renew request watchdog timer
    return retval;
}


/*
 * \brief Replace user defined names with internal names
 */
std::map <std::string, std::string>sanitize_row_ext_names (
    const CsvMap &cm,
    size_t row_i,
    bool sanitize
)
{
    std::map <std::string, std::string> result;
    // make copy of this one line
    for (auto title: cm.getTitles ()) {
        result[title] = cm.get(row_i, title);
    }
    if (sanitize) {
        // sanitize ext names to t_bios_asset_element.name
        auto sanitizeList = {"location", "logical_asset", "power_source.", "group." };
        for (auto item: sanitizeList) {
            if (item [strlen (item) - 1] == '.') {
                // iterate index .X
                for (int i = 1; true; ++i) {
                    std::string title = item + std::to_string (i);
                    auto it = result.find (title);
                    if (it == result.end ()) break;

                    std::string name;
                    int rv = DBAssets::extname_to_asset_name (it->second, name);
                    if (rv != 0) { name = it->second; }
                    log_debug ("sanitized %s '%s' -> '%s'", title.c_str(), it->second.c_str(), name.c_str ());
                    result [title] = name;
                }
            } else {
                // simple name
                auto it = result.find (item);
                if (it != result.end ()) {
                    std::string name;
                    int rv = DBAssets::extname_to_asset_name (it->second, name);
                    if (rv != 0) { name = it->second; }
                    log_debug ("sanitized %s '%s' -> '%s'", it->first.c_str (), it->second.c_str(), name.c_str ());
                    result [item] = name;
                }
            }
        }
    }
    return result;
}


/*
 * \brief Request licensing limitation
 * TODO if there will be more limitations, fix return value to struct instead of int
 *
 */

void get_licensing_limitation(LIMITATIONS_STRUCT &limitations)
{
    // default values
    limitations.max_active_power_devices = -1;
    limitations.global_configurability = 0;
    // query values
    MlmClientPool::Ptr client_ptr = mlm_pool.get ();
    zmsg_t *request = zmsg_new();
    zmsg_addstr (request, "LIMITATION_QUERY");
    zuuid_t *zuuid = zuuid_new ();
    const char *zuuid_str = zuuid_str_canonical (zuuid);
    zmsg_addstr (request, zuuid_str);
    zmsg_addstr (request, "*");
    zmsg_addstr (request, "*");
    int rv = client_ptr->sendto ("etn-licensing", "LIMITATION_QUERY", 1000, &request);
    if (rv == -1) {
        zuuid_destroy (&zuuid);
        zmsg_destroy (&request);
        log_fatal ("Cannot send message to etn-licensing");
        std::string err = TRANSLATE_ME ("mlm_client_sendto failed.");
        bios_throw ("internal-error", err.c_str ());
    }

    zmsg_t *response = client_ptr->recv (zuuid_str, 30);
    zuuid_destroy (&zuuid);
    if (!response) {
        log_fatal ("client->recv (timeout = '30') returned NULL for LIMITATION_QUERY");
        std::string err = TRANSLATE_ME ("client->recv () returned NULL");
        bios_throw ("internal-error", err.c_str ());
    }
    char *reply = zmsg_popstr (response);
    char *status = zmsg_popstr (response);
    if (streq (status, "OK") && streq (reply, "REPLY")) {
        zmsg_t *submsg = zmsg_popmsg(response);
        while (submsg) {
            fty_proto_t *submetric = fty_proto_decode(&submsg);
            assert (fty_proto_id(submetric) == FTY_PROTO_METRIC);
            if (streq (fty_proto_name(submetric), "rackcontroller-0") && streq (fty_proto_type(submetric), "power_nodes.max_active")) {
                limitations.max_active_power_devices = atoi(fty_proto_value (submetric));
                log_debug("limitations.max_active_power_device set to %i", limitations.max_active_power_devices);
            }
            else if (streq (fty_proto_name(submetric), "rackcontroller-0") && streq (fty_proto_type(submetric), "configurability.global")) {
                limitations.global_configurability = atoi(fty_proto_value (submetric));
                log_debug("limitations.global_configurability set to %i", limitations.global_configurability);
            }
            fty_proto_destroy(&submetric);
            submsg = zmsg_popmsg(response);
        }
    }
    zstr_free (&reply);
    zstr_free (&status);
    zmsg_destroy (&response);
}


/*
 * \brief Processes a single row from csv file
 *
 * \param[in] conn     - a connection to DB
 * \param[in] cm       - already parsed csv file
 * \param[in] row_i    - number of row to process
 * \param[in] TYPES    - list of available types
 * \param[in] SUBTYPES - list of available subtypes
 * \param[in][out] ids - list of already seen asset ids
 *
 */
static std::pair<db_a_elmnt_t, persist::asset_operation>
    process_row
        (tntdb::Connection &conn,
         const CsvMap &cm,
         size_t row_i,
         const std::map<std::string,int> &TYPES,
         const std::map<std::string,int> &SUBTYPES,
         std::set<a_elmnt_id_t> &ids,
         bool sanitize,
         size_t rc_0,
         LIMITATIONS_STRUCT limitations
         )
{
    LOG_START;

    log_debug ("################ Row number is %zu", row_i);
    static const std::set<std::string> STATUSES = \
        { "active", "nonactive", "spare", "retired"};

    if (0 == limitations.global_configurability) {
        std::string action = TRANSLATE_ME ("Asset handling");
        std::string reason = TRANSLATE_ME ("Licensing global_configurability limit hit");
        bios_throw("action-forbidden", action.c_str (), reason.c_str ());
    }

    // get location, powersource etc as name from ext.name
    auto sanitizedAssetNames = sanitize_row_ext_names (cm, row_i, sanitize);

    // This is used to track, which columns had been already processed,
    // because if they was't processed yet,
    // then they should be treated as external attributes
    auto unused_columns = cm.getTitles();

    if (unused_columns.empty()) {
        std::string err = TRANSLATE_ME ("Cannot import empty document.");
        bios_throw("bad-request-document", err.c_str ());
    }

    // remove the column 'create_mode' which is set to a different value anyway
    if (unused_columns.count("create_mode"))
        unused_columns.erase ("create_mode");

   // because id is definitely not an external attribute
    auto id_str = unused_columns.count("id") ? cm.get(row_i, "id") : "";
    if (rc_0 != row_i && "rackcontroller-0" == id_str && rc_0 != std::numeric_limits<std::size_t>::max() ) {
        // we got RC-0 but it don't match "myself", change it to something else ("")
        log_debug("RC is marked as rackcontroller-0, but it's not myself");
        id_str = "";
    } else if (rc_0 == row_i && id_str != "rackcontroller-0") {
        log_debug("RC is identified as rackcontroller-0");
        id_str = "rackcontroller-0";
    }

    unused_columns.erase("id");
    persist::asset_operation operation = persist::asset_operation::INSERT;
    int64_t id = 0;
    if ( !id_str.empty() )
    {
        id = DBAssets::name_to_asset_id (id_str);
        if (id == -1) {
            bios_throw("element-not-found", id_str.c_str ());
        }
        if ( ids.count(id) == 1 ) {
            std::string msg = TRANSLATE_ME ("Element id '%s' found twice, aborting", id_str.c_str ());
            bios_throw("bad-request-document", msg.c_str());
        }
        /*
         * removed ids insert here as we only want to pair good rows
        ids.insert(id);
        */
        operation = persist::asset_operation::UPDATE;
    }

    auto ename = cm.get(row_i, "name");
    if (ename.empty ()) {
        std::string received = TRANSLATE_ME ("empty value");
        std::string expected = TRANSLATE_ME ("unique, non empty value");
        bios_throw("request-param-bad", "name", received.c_str (), expected.c_str ());
    }
    if (ename.length() > 50) {
        std::string received = TRANSLATE_ME ("too long string");
        std::string expected = TRANSLATE_ME ("unique string from 1 to 50 characters");
        bios_throw("request-param-bad", "name", received.c_str (), expected.c_str ());
    }
    std::string iname;
    int rv = DBAssets::extname_to_asset_name (ename, iname);
    if (!id_str.empty () && rv == 0) {
        // internal name from DB must be the same as internal name from CSV
        if (iname != id_str) {
            std::string received = TRANSLATE_ME ("already existing name");
            std::string expected = TRANSLATE_ME ("unique string from 1 to 50 characters");
            bios_throw("request-param-bad", "name", received.c_str (), expected.c_str ());
        }
    }
    log_debug ("name = '%s/%s'", ename.c_str(), iname.c_str());
    if (rv == -2) {
        std::string err = TRANSLATE_ME ("Database failure");
        bios_throw("internal-error", err.c_str ());
    }
    unused_columns.erase("name");

    auto type = cm.get_strip(row_i, "type");
    log_debug ("type = '%s'", type.c_str());
    if ( TYPES.find(type) == TYPES.end() ) {
        std::string received = type.empty() ? TRANSLATE_ME ("empty value") : JSONIFY (type.c_str());
        std::string expected = JSONIFY(utils::join_keys_map(TYPES, ", ").c_str());
        bios_throw("request-param-bad", "type", received.c_str (), expected.c_str ());
    }
    auto type_id = TYPES.find(type)->second;
    unused_columns.erase("type");

    auto status = cm.get_strip(row_i, "status");
    log_debug ("status = '%s'", status.c_str());
    if ( STATUSES.find(status) == STATUSES.end() ) {
        std::string received = status.empty() ? TRANSLATE_ME ("empty value") : JSONIFY (status.c_str());
        std::string expected = JSONIFY(cxxtools::join(STATUSES.cbegin(), STATUSES.cend(), ", ").c_str());
        bios_throw ("request-param-bad", "status", received.c_str (), expected.c_str ());
    }
    unused_columns.erase("status");

    auto asset_tag =  unused_columns.count("asset_tag") ? cm.get(row_i, "asset_tag") : "";
    log_debug ("asset_tag = '%s'", asset_tag.c_str());
    if (asset_tag.length() > 50) {
        std::string received = TRANSLATE_ME ("too long string");
        std::string expected = TRANSLATE_ME ("unique string from 1 to 50 characters");
        bios_throw("request-param-bad", "asset_tag", received.c_str (), expected.c_str ());
    }
    unused_columns.erase("asset_tag");

    int priority = get_priority(cm.get_strip(row_i, "priority"));
    log_debug ("priority = %d", priority);
    unused_columns.erase("priority");

    auto location = sanitizedAssetNames ["location"];
    log_debug ("location = '%s'", location.c_str());
    a_elmnt_id_t parent_id = 0;
    if ( !location.empty() )
    {
        auto ret = select_asset_element_by_name(conn, location.c_str());
        if ( ret.status == 1 )
            parent_id = ret.item.id;
        else {
            if (ret.errsubtype == DB_ERROR_NOTFOUND) {
                std::string expected = TRANSLATE_ME ("<existing asset name>");
                bios_throw("request-param-bad", "location", location.c_str(), expected.c_str ());
            }
            else {
                std::string err = TRANSLATE_ME ("Database failure");
                bios_throw("internal-error", err.c_str ());
            }
        }
    }
    unused_columns.erase("location");

    // Business requirement: be able to write 'rack controller', 'RC', 'rc' as subtype == 'rack controller'
    std::map<std::string,int> local_SUBTYPES = SUBTYPES;
    int rack_controller_id = SUBTYPES.find ("rack controller")->second;
    int patch_panel_id = SUBTYPES.find ("patch panel")->second;

    local_SUBTYPES.emplace (std::make_pair ("rackcontroller", rack_controller_id));
    local_SUBTYPES.emplace (std::make_pair ("rackcontroler", rack_controller_id));
    local_SUBTYPES.emplace (std::make_pair ("rc", rack_controller_id));
    local_SUBTYPES.emplace (std::make_pair ("RC", rack_controller_id));
    local_SUBTYPES.emplace (std::make_pair ("RC3", rack_controller_id));

    local_SUBTYPES.emplace (std::make_pair ("patchpanel", patch_panel_id));

    auto subtype = cm.get_strip (row_i, "sub_type");

    log_debug ("subtype = '%s'", subtype.c_str());
    if ( ( type == "device" ) &&
         ( local_SUBTYPES.find(subtype) == local_SUBTYPES.cend() ) ) {
        std::string received = subtype.empty() ? TRANSLATE_ME ("empty value") : JSONIFY (subtype.c_str());
        std::string expected = JSONIFY(utils::join_keys_map(SUBTYPES, ", ").c_str());
        bios_throw("request-param-bad", "subtype", received.c_str (), expected.c_str ());
    }

    if ( ( !subtype.empty() ) && ( type != "device" ) && ( type != "group") )
    {
        log_warning ("'%s' - subtype is ignored", subtype.c_str());
    }

    if ( ( subtype.empty() ) && ( type == "group" ) ) {
        std::string expected = TRANSLATE_ME ("subtype (for type group)");
        bios_throw("request-param-required", expected.c_str ());
    }

    auto subtype_id = local_SUBTYPES.find(subtype)->second;
    unused_columns.erase("sub_type");

    // since we have all the data about the asset, licensing check could be done now
    /*if (-1 != limitations.max_active_power_devices && "active" == status && TYPES.find("device")->second == type_id) {
        std::string db_status = DBAssets::get_status_from_db (conn, id_str);
        // limit applies only to assets that are attempted to be activated, but are disabled in database
        // or to new assets, also may trigger in case of DB failure, but that's fine
        if (db_status != "active") {
            if ((SUBTYPES.find("epdu")->second == subtype_id
                    || SUBTYPES.find("sts")->second == subtype_id
                    || SUBTYPES.find("ups")->second == subtype_id
                    || SUBTYPES.find("genset")->second == subtype_id
                    || SUBTYPES.find("pdu")->second == subtype_id
                    ) && DBAssets::get_active_power_devices (conn) + 1 > limitations.max_active_power_devices) {
                std::string action = TRANSLATE_ME ("Asset handling");
                std::string reason = TRANSLATE_ME ("Licensing maximum amount of active power devices limit reached");
                bios_throw("action-forbidden", action.c_str (), reason.c_str ());
            }
        }
    }*/

    // now we have read all basic information about element
    // if id is set, then it is right time to check what is going on in DB
    if ( !id_str.empty() )
    {
        db_reply <db_web_basic_element_t> element_in_db = DBAssets::select_asset_element_web_byId
                                                        (conn, id);
        if ( element_in_db.status == 0 ) {
            if (element_in_db.errsubtype == DB_ERROR_NOTFOUND) {
                bios_throw("element-not-found", id_str.c_str());
            }
            else {
                std::string err = TRANSLATE_ME ("Database failure");
                bios_throw("internal-error", err.c_str ());
            }
        }
        else
        {
            if ( element_in_db.item.type_id != type_id ) {
                std::string err = TRANSLATE_ME ("Changing of asset type is forbidden");
                bios_throw("bad-request-document", err.c_str ());
            }
            if ( ( element_in_db.item.subtype_id != subtype_id ) &&
                 ( element_in_db.item.subtype_name != "N_A" ) ) {
                std::string err = TRANSLATE_ME ("Changing of asset subtype is forbidden");
                bios_throw("bad-request-document", err.c_str ());
            }
        }
    }

    std::string group;

    // list of element ids of all groups, the element belongs to
    std::set <a_elmnt_id_t>  groups{};
    for ( int group_index = 1 ; true; group_index++ )
    {
        std::string grp_col_name = "";
        try {
            // column name
            grp_col_name = "group." + std::to_string(group_index);
            // remove from unused
            unused_columns.erase(grp_col_name);
            // take value
            group = sanitizedAssetNames.at (grp_col_name);
        }
        catch (const std::out_of_range &e)
        // if column doesn't exist, then break the cycle
        {
            log_debug ("end of group processing");
            log_debug ("%s", e.what());
            break;
        }
        log_debug ("group_name = '%s'", group.c_str());
        // if group was not specified, just skip it
        if ( !group.empty() )
        {
            // find an id from DB
            auto ret = select_asset_element_by_name(conn, group.c_str());
            if ( ret.status == 1 )
                groups.insert(ret.item.id);  // if OK, then take ID
            else
            {
                if (ret.errsubtype == DB_ERROR_NOTFOUND) {
                    log_error ("group '%s' is not present in DB, rejected", group.c_str());
                    bios_throw("element-not-found", group.c_str());
                }
                else {
                    std::string err = TRANSLATE_ME ("Database failure");
                    bios_throw("internal-error", err.c_str ());
                }
            }
        }
    }

    std::vector <link_t>  links{};
    std::string  link_source = "";
    for ( int link_index = 1; true; link_index++ )
    {
        link_t one_link{0, 0, NULL, NULL, 0};
        std::string link_col_name = "";
        try {
            // column name
            link_col_name = "power_source." +
                                                std::to_string(link_index);
            // remove from unused
            unused_columns.erase(link_col_name);
            // take value
            link_source = sanitizedAssetNames.at (link_col_name);
        }
        catch (const std::out_of_range &e)
        // if column doesn't exist, then break the cycle
        {
            log_debug ("end of power links processing");
            log_debug ("%s", e.what());
            break;
        }

        // prevent power source being myself
        if (link_source == ename) {
                log_debug("Ignoring power source=myself");
                link_source = "";
                auto link_col_name1 = "power_plug_src." + std::to_string(link_index);
                auto link_col_name2 = "power_input." + std::to_string(link_index);
                if (!link_col_name1.empty()) unused_columns.erase(link_col_name1);
                if (!link_col_name2.empty()) unused_columns.erase(link_col_name2);
                continue;
        }

        log_debug ("power_source_name = '%s'", link_source.c_str());
        if ( !link_source.empty() ) // if power source is not specified
        {
            // find an id from DB
            auto ret = select_asset_element_by_name
                (conn, link_source.c_str());
            if ( ret.status == 1 )
                one_link.src = ret.item.id;  // if OK, then take ID
            else
            {
                if (ret.errsubtype == DB_ERROR_NOTFOUND) {
                    log_warning ("power source '%s' is not present in DB, rejected",
                    link_source.c_str());
                    bios_throw("element-not-found", link_source.c_str());
                }
                else {
                    std::string err = TRANSLATE_ME ("Database failure");
                    bios_throw("internal-error", err.c_str ());
                }
            }
        }

        // column name
        auto link_col_name1 = "power_plug_src." + std::to_string(link_index);
        try{
            // remove from unused
            unused_columns.erase(link_col_name1);
            // take value
            auto link_source1 = cm.get(row_i, link_col_name1);
            // TODO: bad idea, char = byte
            // FIXME: THIS IS MEMORY LEAK!!!
            one_link.src_out = new char [4];
            strcpy ( one_link.src_out, link_source1.substr (0,4).c_str());
        }
        catch (const std::out_of_range &e)
        {
            log_debug ("'%s' - is missing at all", link_col_name1.c_str());
            log_debug ("%s", e.what());
        }

        // column name
        auto link_col_name2 = "power_input." + std::to_string(link_index);
        try{
            unused_columns.erase(link_col_name2); // remove from unused
            auto link_source2 = cm.get(row_i, link_col_name2);// take value
            // TODO: bad idea, char = byte
            // FIXME: THIS IS MEMORY LEAK!!!
            one_link.dest_in = new char [4];
            strcpy ( one_link.dest_in, link_source2.substr (0,4).c_str());
        }
        catch (const std::out_of_range &e)
        {
            log_debug ("'%s' - is missing at all", link_col_name2.c_str());
            log_debug ("%s", e.what());
        }

        if ( one_link.src != 0 ) // if first column was ok
        {
            if ( type == "device" )
            {
                one_link.type = 1; // TODO remove hardcoded constant
                links.push_back(one_link);
            }
            else
            {
                log_warning ("information about power sources is ignored for type '%s'", type.c_str());
            }
        }
    }

    // sanity check, for RC-0 always skip HW attributes
    if (rc_0 == row_i && id_str != "rackcontroller-0") {
        int i;
        // remove all ip.X
        for (i = 1; ; ++i) {
            std::string what = "ip." + std::to_string(i);
            if (unused_columns.count(what)) {
                unused_columns.erase(what); // remove from unused
            } else {
                break;
            }
        }
        // remove all ipv6.X
        for (i = 1; ; ++i) {
            std::string what = "ipv6." + std::to_string(i);
            if (unused_columns.count(what)) {
                unused_columns.erase(what); // remove from unused
            } else {
                break;
            }
        }
        // remove fqdn
        if (unused_columns.count("fqdn")) {
            unused_columns.erase("fqdn"); // remove from unused
        }
        // remove serial_no
        if (unused_columns.count("serial_no")) {
            unused_columns.erase("serial_no"); // remove from unused
        }
        // remove model
        if (unused_columns.count("model")) {
            unused_columns.erase("model"); // remove from unused
        }
        // remove manufacturer
        if (unused_columns.count("manufacturer")) {
            unused_columns.erase("manufacturer"); // remove from unused
        }
        // remove uuid
        if (unused_columns.count("uuid")) {
            unused_columns.erase("uuid"); // remove from unused
        }
    }

    _scoped_zhash_t *extattributes = zhash_new();
    zhash_autofree(extattributes);
    zhash_insert (extattributes, "name", (void *) ename.c_str ());
    for ( auto &key: unused_columns )
    {
        // try is not needed, because here are keys that are definitely there
        std::string value = cm.get(row_i, key);

        // BIOS-1564: sanitize the date for warranty_end -- start
        if (is_date (key) && !value.empty()) {
            char *date = sanitize_date (value.c_str());
            if (!date) {
                log_info ("Cannot sanitize %s '%s' for device '%s'", key.c_str(), value.c_str(), ename.c_str());
                std::string expected = TRANSLATE_ME("ISO date");
                bios_throw("request-param-bad", key.c_str(), value.c_str(), expected.c_str ());
            }
            value = date;
            zstr_free (&date);
        }
        // BIOS-1564 -- end

        // BIOS-2302: Check some attributes for sensors
        // BIOS-2784: Check max_current, max_power
        if ( key == "logical_asset" && !value.empty() ) {
            // check, that this asset exists

            value = sanitizedAssetNames.at ("logical_asset");

            auto ret = select_asset_element_by_name
                (conn, value.c_str());
            if ( ret.status == 0 ) {
                if (ret.errsubtype == DB_ERROR_NOTFOUND) {
                    log_info ("logical_asset '%s' does not present in DB, rejected",
                    value.c_str());
                    bios_throw("element-not-found", value.c_str());
                }
                else {
                    std::string err = TRANSLATE_ME ("Database failure");
                    bios_throw("internal-error", err.c_str ());
                }

                log_info ("logical_asset '%s' does not present in DB, rejected",
                    value.c_str());
                bios_throw("element-not-found", value.c_str());
            }
        }
        else
        if (    ( key == "calibration_offset_t" || key == "calibration_offset_h" )
             &&   !value.empty() )
        {
            // we want exceptions to propagate to upper layer
            sanitize_value_double (key, value);
        }
        else
        if (    ( key == "max_current" || key == "max_power" )
             &&   !value.empty() )
        {
            // we want exceptions to propagate to upper layer
            double d_value = sanitize_value_double (key, value);
            if ( d_value < 0 ) {
                log_info ("Extattribute: %s='%s' is neither positive not zero", key.c_str(), value.c_str());
                std::string expected = TRANSLATE_ME ("value must be a not negative number");
                bios_throw ("request-param-bad", key.c_str(), ("'" + value + "'").c_str(), expected.c_str ());
            }
        }
        // BIOS-2781
        if ( key == "location_u_pos" && !value.empty() ) {
            unsigned long ul = 0;
            try {
                std::size_t pos = 0;
                ul = std::stoul (value, &pos);
                if  ( pos != value.length() ) {
                    log_info ("Extattribute: %s='%s' is not unsigned integer", key.c_str(), value.c_str());
                    std::string expected = TRANSLATE_ME ("value must be an unsigned integer");
                    bios_throw ("request-param-bad", key.c_str(), ("'" + value + "'").c_str(), expected.c_str ());
                }
            }
            catch (const std::exception& e) {
                log_info ("Extattribute: %s='%s' is not unsigned integer", key.c_str(), value.c_str());
                std::string expected = TRANSLATE_ME ("value must be an unsigned integer");
                bios_throw ("request-param-bad", "location_u_pos", ("'" + value + "'").c_str (), expected.c_str ());
            }
            if (ul == 0 || ul > 52) {
                std::string expected = TRANSLATE_ME ("value must be between <1, rack size>, where rack size must be <= 52.");
                bios_throw ("request-param-bad", "location_u_pos", ("'" + value + "'").c_str (), expected.c_str ());
            }
        }
        // BIOS-2799
        if ( key == "u_size" && !value.empty() ) {
            unsigned long ul = 0;
            try {
                std::size_t pos = 0;
                ul = std::stoul (value, &pos);
                if  ( pos != value.length() ) {
                    log_info ("Extattribute: %s='%s' is not unsigned integer", key.c_str(), value.c_str());
                    std::string expected = TRANSLATE_ME ("value must be an unsigned integer");
                    bios_throw ("request-param-bad", key.c_str(), ("'" + value + "'").c_str(), expected.c_str ());
                }
            }
            catch (const std::exception& e) {
                log_info ("Extattribute: %s='%s' is not unsigned integer", key.c_str(), value.c_str());
                std::string expected = TRANSLATE_ME ("value must be an unsigned integer");
                bios_throw ("request-param-bad", "u_size", ("'" + value + "'").c_str (), expected.c_str ());
            }
            if (ul == 0 || ul > 52) {
                std::string expected = TRANSLATE_ME ("value must be between <1, rack size>, where rack size must be <= 52.");
                bios_throw ("request-param-bad", "u_size", ("'" + value + "'").c_str (), expected.c_str ());
            }
        }

        if ( match_ext_attr (value, key) )
        {
            // ACE: temporary disabled
            // for testing purposes for rabobank usecase
            // IMHO: There is no sense to check it at all
            //   * as we cannot guarantee that manufacturers in the whole world use UNIQUE serial numbers
            //   * not unique serial number of the device should not forbid users to monitor their devices
            /*
            if ( key == "serial_no" )
            {

                if  ( unique_keytag (conn, key, value, id) == 0 )
                    zhash_insert (extattributes, key.c_str(), (void*)value.c_str());
                else
                {
                    bios_throw("request-param-bad", "serial_no", value.c_str(), "<unique string>");
                }
                continue;
            }
            */
            zhash_insert (extattributes, key.c_str(), (void*)value.c_str());
        }
    }
    // if the row represents group, the subtype represents a type
    // of the group.
    // As group has no special table as device, then this information
    // sould be inserted as external attribute
    // this parametr is mandatory according rfc-11
    if ( type == "group" )
        zhash_insert (extattributes, "type", (void*) subtype.c_str() );

    db_a_elmnt_t m;

    if ( !id_str.empty() )
    {
        _scoped_zhash_t *extattributesRO = zhash_new();
        zhash_autofree(extattributesRO);
        if(cm.getUpdateTs() != "")
            zhash_insert(extattributesRO, "update_ts", (void*) cm.getUpdateTs().c_str());
        if(cm.getUpdateUser() != "")
            zhash_insert(extattributesRO, "update_user", (void*) cm.getUpdateUser().c_str());
        m.id = id;
        std::string errmsg = "";
        if (type != "device" )
        {
            auto ret = update_dc_room_row_rack_group
                (conn, m.id, iname.c_str(), type_id, parent_id,
                 extattributes, status.c_str(), priority, groups, asset_tag, errmsg, extattributesRO);
            if ( ( ret ) || ( !errmsg.empty() ) ) {
                throw std::invalid_argument(errmsg);
            }
        }
        else
        {
            auto ret = update_device
                (conn, m.id, iname.c_str(), type_id, parent_id,
                 extattributes, "nonactive", priority, groups, links, asset_tag, errmsg, extattributesRO);
            if ( ( ret ) || ( !errmsg.empty() ) ) {
                throw std::invalid_argument(errmsg);
            }
        }
    }
    else
    {
            _scoped_zhash_t *extattributesRO = zhash_new();
            zhash_autofree(extattributesRO);
            if(cm.getCreateMode() != 0)
                zhash_insert(extattributesRO, "create_mode", (void*) std::to_string(cm.getCreateMode()).c_str());
            if(cm.getCreateUser() != "")
                zhash_insert(extattributesRO, "create_user", (void*) cm.getCreateUser().c_str());
        if ( type != "device" )
        {
            // this is a transaction
            auto ret = insert_dc_room_row_rack_group
                (conn, ename.c_str(), type_id, parent_id,
                 extattributes, status.c_str(), priority, groups, asset_tag, extattributesRO);
            if ( ret.status != 1 ) {
                throw BiosError(ret.rowid, ret.msg);
            }
            m.id = ret.rowid;
        }
        else
        {
            // this is a transaction
            auto ret = insert_device (conn, links, groups, ename.c_str(),
                    parent_id, extattributes, subtype_id, subtype.c_str(), "nonactive",
                    priority, asset_tag, extattributesRO);
            if ( ret.status != 1 ) {
                throw BiosError(ret.rowid, ret.msg);
            }
            m.id = ret.rowid;
        }
    }
    rv = DBAssets::extname_to_asset_name (ename, m.name);
    if (rv != 0) {
        std::string err = TRANSLATE_ME ("Database failure");
        bios_throw("internal-error", err.c_str ());
    }

    if (type == "device" && status == "active")
    {
        // check if we may activate the device
         try
         {
             std::string asset_json = getJsonAsset (NULL, m.id);
             mlm::MlmSyncClient client (AGENT_FTY_ASSET, AGENT_ASSET_ACTIVATOR);
             fty::AssetActivator activationAccessor (client);
             activationAccessor.activate (asset_json);
         }
         catch (const std::exception &e)
         {
             std::string err = JSONIFY (e.what());
             bios_throw ("licensing-err", err.c_str ())
         }

    }
    m.status = status;
    m.parent_id = parent_id;
    m.priority = priority;
    m.type_id = type_id;
    m.subtype_id = subtype_id;
    m.asset_tag = asset_tag;

    for (void* it = zhash_first (extattributes); it != NULL;
               it = zhash_next (extattributes)) {
        m.ext.emplace (zhash_cursor (extattributes), (char*) it);
    }

    if ( !id_str.empty() ) {
        ids.insert(id);
    }

    LOG_END;
    return std::make_pair(m, operation) ;
}


/*
 * \brief Checks if mandatory columns are missing in csv file
 *
 * This check is implemented according BAM DC010
 *
 * \param cm - already parsed csv file
 *
 * \return emtpy string if everything is ok, otherwise the name of missing row
 */
//MVY: moved out to support friendly error messages below
static std::vector<std::string> MANDATORY = {
    "name", "type", "sub_type", "location", "status",
    "priority"
};
static std::string
mandatory_missing
        (const CsvMap &cm)
{
    auto all_fields = cm.getTitles();
    for (const auto& s : MANDATORY) {
        if (all_fields.count(s) == 0)
            return s;
    }

    return "";
}

void
    load_asset_csv
        (std::istream& input,
         std::vector <std::pair<db_a_elmnt_t,persist::asset_operation>> &okRows,
         std::map <int, std::string> &failRows,
         touch_cb_t touch_fn,
         std::string user
         )
{
    LOG_START;

    std::vector <std::vector<cxxtools::String> > data;
    cxxtools::CsvDeserializer deserializer(input);
    char delimiter = findDelimiter(input);
    if (delimiter == '\x0') {
        std::string msg{TRANSLATE_ME ("Cannot detect the delimiter, use comma (,) semicolon (;) or tabulator")};
        log_error("%s", msg.c_str());
        LOG_END;
        bios_throw("bad-request-document", msg.c_str());
    }
    log_debug("Using delimiter '%c'", delimiter);
    deserializer.delimiter(delimiter);
    deserializer.readTitle(false);
    deserializer.deserialize(data);
    CsvMap cm{data};
    cm.deserialize();

    cm.setCreateMode(CREATE_MODE_CSV);
    cm.setCreateUser(user);
    cm.setUpdateUser(user);
    std::time_t timestamp = std::time(NULL);
    char mbstr[100];
    if (std::strftime(mbstr, sizeof (mbstr), "%FT%T%z", std::localtime(&timestamp))) {
        cm.setUpdateTs(std::string(mbstr));
    }
    return load_asset_csv(cm, okRows, failRows, touch_fn);
}

std::pair<db_a_elmnt_t, persist::asset_operation>
    process_one_asset
        (const CsvMap& cm)
{
    LOG_START;

    auto m = mandatory_missing(cm);
    if ( m != "" )
    {
        std::string msg{"column '" + m + "' is missing, import is aborted"};
        log_error("%s", msg.c_str());
        LOG_END;
        bios_throw("request-param-required", m.c_str());
    }

    tntdb::Connection conn;
    std::string msg{"No connection to database"};
    try{
        conn = tntdb::connectCached(DBConn::url);
    }
    catch(...)
    {
        log_error("%s", msg.c_str());
        LOG_END;
        bios_throw("internal-error", msg.c_str());
    }

    auto TYPES = read_element_types (conn);
    if (TYPES.empty ())
        bios_throw("internal-error", msg.c_str());

    auto SUBTYPES = read_device_types (conn);
    if (SUBTYPES.empty ())
        bios_throw("internal-error", msg.c_str());

    std::set<a_elmnt_id_t> ids{};
    int rc_0 = -1;
    auto unused_columns = cm.getTitles();
    if (unused_columns.empty()) {
        bios_throw("bad-request-document", "Cannot import empty document.");
    }
    std::string iname = unused_columns.count("id") ? cm.get(1, "id") : "noid";
    if ("rackcontroller-0" == iname) {
        log_debug("RC-0 detected");
        rc_0 = 1;
    } else {
        log_debug("RC-0 not detected");
        rc_0 = -1;
    }
    LIMITATIONS_STRUCT limitations;
    get_licensing_limitation(limitations);
    auto ret = process_row(conn, cm, 1, TYPES, SUBTYPES, ids, true, rc_0, limitations);
    LOG_END;
    return ret;
}

void
    load_asset_csv
        (const CsvMap& cm,
         std::vector <std::pair<db_a_elmnt_t,persist::asset_operation>> &okRows,
         std::map <int, std::string> &failRows,
         touch_cb_t touch_fn
         )
{
    LOG_START;

    auto m = mandatory_missing(cm);
    if ( m != "" )
    {
        std::string msg{"column '" + m + "' is missing, import is aborted"};
        log_error("%s", msg.c_str());
        LOG_END;
        std::string msg_received = TRANSLATE_ME ("<missing column '%s'>", m.c_str ());
        std::string msg_expected = TRANSLATE_ME ("<column '%s' is present in csv>", m.c_str ());
        bios_throw("request-param-bad", m.c_str(), msg_received.c_str(), msg_expected.c_str());
    }

    tntdb::Connection conn;
    std::string msg{TRANSLATE_ME ("No connection to database")};
    try{
        conn = tntdb::connectCached(DBConn::url);
    }
    catch(...)
    {
        log_error("%s", msg.c_str());
        LOG_END;
        bios_throw("internal-error", msg.c_str());
    }

    auto TYPES = read_element_types (conn);

    if (TYPES.empty ())
        bios_throw("internal-error", msg.c_str());

    auto SUBTYPES = read_device_types (conn);
    if (SUBTYPES.empty ())
        bios_throw("internal-error", msg.c_str());

    // BIOS-2506
    std::set<a_elmnt_id_t> ids{};

    // if there are rackcontroller in the import, promote one of it to rackcontroller-0 if not done already
    MlmClient client;
    // rc0 promotion disabled due to multiple datacenter support
    // - hard to decide whether it's import of DC after crash or another DC, RC location must be set manually
    size_t rc0 = std::numeric_limits<std::size_t>::max();
    //int rc0 = promote_rc0(&client, cm, touch_fn);
    // get licensing limitation, if any
    LIMITATIONS_STRUCT limitations;
    get_licensing_limitation(limitations);

    std::set<size_t> processedRows;
    bool somethingProcessed;
    do {
        somethingProcessed = false;
        failRows.clear ();
        for (size_t row_i = 1; row_i != cm.rows(); ++row_i) {
            if (processedRows.find (row_i) != processedRows.end ()) continue;
            try{
                auto ret = process_row(conn, cm, row_i, TYPES, SUBTYPES, ids, true, rc0, limitations);
                touch_fn ();
                okRows.push_back (ret);
                log_info ("row %zu was imported successfully", row_i);
                somethingProcessed = true;
                processedRows.insert (row_i);
            }
            catch (const std::invalid_argument &e) {
                failRows.insert(std::make_pair(row_i + 1, e.what()));
                log_error ("row %zu not imported: %s", row_i, e.what());
            }
        }
    } while (somethingProcessed);
    LOG_END;
}

} // namespace persist
