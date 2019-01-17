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

/*!
 * \file data.cc
 * \author Alena Chernikava <AlenaChernikava@Eaton.com>
 * \author Michal Vyskocil <MichalVyskocil@Eaton.com>
 * \author Michal Hrusecky <MichalHrusecky@Eaton.com>
 * \author Karol Hrdina <KarolHrdina@Eaton.com>
 * \author Tomas Halman <TomasHalman@Eaton.com>
 * \author Jim Klimov <EvgenyKlimov@Eaton.com>
 * \brief Not yet documented file
 */
#include <algorithm>
#include <fty_common_db_dbpath.h>
#include <fty_common_rest.h>
#include <fty_common.h>
#include <fty_common_macros.h>

#include "shared/data.h"

#include "../db/asset_general.h"

static std::vector<std::tuple <a_elmnt_id_t, std::string, std::string, std::string>>
s_get_parents (tntdb::Connection &conn, a_elmnt_id_t id)
{

    std::vector<std::tuple <a_elmnt_id_t, std::string, std::string, std::string>> ret {};

    std::function<void(const tntdb::Row&)> cb = \
        [&ret](const tntdb::Row &row) {

            // C++ is c r a z y!! Having static initializer in lambda function made
            // my life easier here, but I did not expected this will work!!
            static const std::vector <std::tuple <std::string, std::string, std::string, std::string>> NAMES = {\
                std::make_tuple ("id_parent1", "parent_name1", "id_type_parent1", "id_subtype_parent1"),
                std::make_tuple ("id_parent2", "parent_name2", "id_type_parent2", "id_subtype_parent2"),
                std::make_tuple ("id_parent3", "parent_name3", "id_type_parent3", "id_subtype_parent3"),
                std::make_tuple ("id_parent4", "parent_name4", "id_type_parent4", "id_subtype_parent4"),
                std::make_tuple ("id_parent5", "parent_name5", "id_type_parent5", "id_subtype_parent5"),
                std::make_tuple ("id_parent6", "parent_name6", "id_type_parent6", "id_subtype_parent6"),
                std::make_tuple ("id_parent7", "parent_name7", "id_type_parent7", "id_subtype_parent7"),
                std::make_tuple ("id_parent8", "parent_name8", "id_type_parent8", "id_subtype_parent8"),
                std::make_tuple ("id_parent9", "parent_name9", "id_type_parent9", "id_subtype_parent9"),
                std::make_tuple ("id_parent10", "parent_name10", "id_type_parent10", "id_subtype_parent10"),
            };

            for (const auto& it: NAMES) {
                a_elmnt_id_t id = 0;
                row [std::get<0> (it)].get (id);
                std::string name;
                row [std::get<1> (it)].get (name);
                a_elmnt_tp_id_t id_type = 0;
                row [std::get<2> (it)].get (id_type);
                a_elmnt_stp_id_t id_subtype = 0;
                row [std::get<3> (it)].get (id_subtype);
                if (!name.empty ())
                    ret.push_back (std::make_tuple (
                        id,
                        name,
                        persist::typeid_to_type (id_type),
                        persist::subtypeid_to_subtype (id_subtype)
                    ));
            }
    };

    int r = DBAssets::select_asset_element_super_parent (conn, id, cb);
    if (r == -1) {
        log_error ("select_asset_element_super_parent failed");
        throw std::runtime_error (TRANSLATE_ME("DBAssets::select_asset_element_super_parent () failed."));
    }

    return ret;
}

db_reply <db_web_element_t>
    asset_manager::get_item1
        (uint32_t id)
{
    db_reply <db_web_element_t> ret;

    try{
        tntdb::Connection conn = tntdb::connectCached(DBConn::url);
        log_debug ("connection was successful");

        auto basic_ret = DBAssets::select_asset_element_web_byId(conn, id);
        log_debug ("1/5 basic select is done");

        if ( basic_ret.status == 0 )
        {
            ret.status        = basic_ret.status;
            ret.errtype       = basic_ret.errtype;
            ret.errsubtype    = basic_ret.errsubtype;
            ret.msg           = JSONIFY(basic_ret.msg.c_str ());
            log_warning ("%s", ret.msg.c_str());
            return ret;
        }
        log_debug ("    1/5 no errors");
        ret.item.basic = basic_ret.item;

        auto ext_ret = DBAssets::select_ext_attributes(conn, id);
        log_debug ("2/5 ext select is done");

        if ( ext_ret.status == 0 )
        {
            ret.status        = ext_ret.status;
            ret.errtype       = ext_ret.errtype;
            ret.errsubtype    = ext_ret.errsubtype;
            ret.msg           = JSONIFY(ext_ret.msg.c_str ());
            log_warning ("%s", ret.msg.c_str());
            return ret;
        }
        log_debug ("    2/5 no errors");
        ret.item.ext = ext_ret.item;

        auto group_ret = DBAssets::select_asset_element_groups(conn, id);
        log_debug ("3/5 groups select is done, but next one is only for devices");

        if ( group_ret.status == 0 )
        {
            ret.status        = group_ret.status;
            ret.errtype       = group_ret.errtype;
            ret.errsubtype    = group_ret.errsubtype;
            ret.msg           = JSONIFY(group_ret.msg.c_str ());
            log_warning ("%s", ret.msg.c_str());
            return ret;
        }
        log_debug ("    3/5 no errors");
        ret.item.groups = group_ret.item;

        if ( ret.item.basic.type_id == persist::asset_type::DEVICE )
        {
            auto powers = DBAssets::select_asset_device_links_to (conn, id, INPUT_POWER_CHAIN);
            log_debug ("4/5 powers select is done");

            if ( powers.status == 0 )
            {
                ret.status        = powers.status;
                ret.errtype       = powers.errtype;
                ret.errsubtype    = powers.errsubtype;
                ret.msg           = JSONIFY(powers.msg.c_str ());
                log_warning ("%s", ret.msg.c_str());
                return ret;
            }
            log_debug ("    4/5 no errors");
            ret.item.powers = powers.item;
        }

        // parents select
        log_debug ("5/5 parents select");
        ret.item.parents = s_get_parents (conn, id);
        log_debug ("     5/5 no errors");

        ret.status = 1;
        return ret;
    }
    catch (const std::exception &e) {
        ret.status        = 0;
        ret.errtype       = DB_ERR;
        ret.errsubtype    = DB_ERROR_INTERNAL;
        ret.msg           = JSONIFY(e.what());
        LOG_END_ABNORMAL(e);
        return ret;
    }
}

db_reply <std::map <uint32_t, std::string> >
    asset_manager::get_items1(
        const std::string &typeName,
        const std::string &subtypeName)
{
    LOG_START;
    log_debug ("subtypename = '%s', typename = '%s'", subtypeName.c_str(),
                    typeName.c_str());
    a_elmnt_stp_id_t subtype_id = 0;
    db_reply <std::map <uint32_t, std::string> > ret;

    a_elmnt_tp_id_t type_id = persist::type_to_typeid(typeName);
    if ( type_id == persist::asset_type::TUNKNOWN ) {
        ret.status        = 0;
        ret.errtype       = DB_ERR;
        ret.errsubtype    = DB_ERROR_INTERNAL;
        ret.msg           = TRANSLATE_ME("Unsupported type of the elements");
        log_error ("%s", ret.msg.c_str());
        // TODO need to have some more precise list of types, so we don't have to change anything here,
        // if something was changed
        bios_error_idx(ret.rowid, ret.msg, "request-param-bad", "type", typeName.c_str(), "datacenters,rooms,ros,racks,devices");
        return ret;
    }
    if ( ( typeName == "device" ) && ( !subtypeName.empty() ) )
    {
        subtype_id = persist::subtype_to_subtypeid(subtypeName);
        if ( subtype_id == persist::asset_subtype::SUNKNOWN ) {
            ret.status        = 0;
            ret.errtype       = DB_ERR;
            ret.errsubtype    = DB_ERROR_INTERNAL;
            ret.msg           = TRANSLATE_ME("Unsupported subtype of the elements");
            log_error ("%s", ret.msg.c_str());
            // TODO need to have some more precise list of types, so we don't have to change anything here,
            // if something was changed
            bios_error_idx(ret.rowid, ret.msg, "request-param-bad", "subtype", subtypeName.c_str(), "ups, epdu, pdu, genset, sts, server, feed");
            return ret;
        }
    }
    log_debug ("subtypeid = %" PRIi16 " typeid = %" PRIi16, subtype_id, type_id);

    try{
        tntdb::Connection conn = tntdb::connectCached(DBConn::url);
        ret = DBAssets::select_short_elements(conn, type_id, subtype_id);
        if ( ret.status == 0 )
            bios_error_idx(ret.rowid, ret.msg, "internal-error", "");
        LOG_END;
        return ret;
    }
    catch (const std::exception &e) {
        LOG_END_ABNORMAL(e);
        ret.status        = 0;
        ret.errtype       = DB_ERR;
        ret.errsubtype    = DB_ERROR_INTERNAL;
        ret.msg           = e.what();
        bios_error_idx(ret.rowid, ret.msg, "internal-error", "");
        return ret;
    }
}

db_reply_t
    asset_manager::delete_item(
        uint32_t id,
        db_a_elmnt_t &element_info)
{
    db_reply_t ret = db_reply_new();

    // As different types should be deleted in differenct way ->
    // find out the type of the element.
    // Even if in old-style the type is already placed in URL,
    // we will ignore it and discover it by ourselves

    try{
        tntdb::Connection conn = tntdb::connectCached(DBConn::url);

        db_reply <db_web_basic_element_t> basic_info =
            DBAssets::select_asset_element_web_byId(conn, id);

        if ( basic_info.status == 0 )
        {
            ret.status        = 0;
            ret.errtype       = basic_info.errsubtype;
            ret.errsubtype    = DB_ERROR_NOTFOUND;
            ret.msg           = TRANSLATE_ME("problem with selecting basic info");
            log_warning ("%s", ret.msg.c_str());
            return ret;
        }
        // here we are only if everything was ok
        element_info.type_id = basic_info.item.type_id;
        element_info.subtype_id = basic_info.item.subtype_id;
        element_info.name = basic_info.item.name;

        // disable deleting RC0
        if (RC0_INAME == element_info.name) {
            log_debug("Prevented deleting RC-0");
            ret.status = 1;
            ret.affected_rows = 0;
            LOG_END;
            return ret;
        }

        // check if a logical_asset refer to the item we are trying to delete
        if (DBAssets::count_keytag(conn,"logical_asset",element_info.name) >0 ){
            ret.status        = 0;
            ret.errtype       = basic_info.errsubtype;
            ret.errsubtype    = DB_ERROR_DELETEFAIL;
            ret.msg           = TRANSLATE_ME("a logical_asset (sensor) refers to it");
            log_warning ("%s", ret.msg.c_str());
            LOG_END;
            return ret;
        }

        switch ( basic_info.item.type_id ) {
            case persist::asset_type::DATACENTER:
            case persist::asset_type::ROW:
            case persist::asset_type::ROOM:
            case persist::asset_type::RACK:
            {
                ret = persist::delete_dc_room_row_rack(conn, id);
                break;
            }
            case persist::asset_type::GROUP:
            {
                ret = persist::delete_group(conn, id);
                break;
            }
            case persist::asset_type::DEVICE:
            {
                ret = persist::delete_device(conn, id);
                break;
            }
            default:
            {
                ret.status        = 0;
                ret.errtype       = basic_info.errsubtype;
                ret.errsubtype    = DB_ERROR_INTERNAL;
                ret.msg           = TRANSLATE_ME("unknown type");
                log_warning ("%s", ret.msg.c_str());
            }
        }
        LOG_END;
        return ret;
    }
    catch (const std::exception &e) {
        ret.status        = 0;
        ret.errtype       = DB_ERR;
        ret.errsubtype    = DB_ERROR_INTERNAL;
        ret.msg           = e.what();
        LOG_END_ABNORMAL(e);
        return ret;
    }
}
