/*
 *
 * Copyright (C) 2015 Eaton
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
 * \file asset_computed_impl.cc
 * \author Tomas Halman <TomasHalman@Eaton.com>
 * \brief Not yet documented file
 */

/*
#include <exception>
#include <tntdb/connection.h>
#include <cxxtools/trim.h>
#include <tntdb/result.h>
#include <tntdb/error.h>
*/
#include <string>
#include <map>
#include <sstream>

#include <tnt/http.h>
#include <tntdb/connection.h>
#include <cxxtools/regex.h>
#include <cxxtools/char.h>
#include <cxxtools/jsonformatter.h>

#include "asset_computed_impl.h"
#include "shared/dbtypes.h"
#include "shared/utils.h"
#include "dbpath.h"
#include "db/assets.h"
#include "asset_types.h"
#include "assetcrud.h"
#include "web_utils.h"
#include "log.h"

int get_devices_usize(
    tntdb::Connection &conn,
    const std::vector<device_info_t> &elements )
{
    int size = 0;

    std::function< void( const tntdb::Row& ) > sumarize = [&size]( const tntdb::Row& row ) {
        uint32_t tmp = string_to_uint32( row.getString("value").c_str() );
        if( tmp != UINT32_MAX ) {
            size += tmp;
        }
    };

    persist::select_asset_ext_attribute_by_keytag( conn, "u_size", elements, sumarize );
    return size;
}

int free_u_size( a_elmnt_id_t elementId, std::string &jsonResult)
{
    try{
        tntdb::Connection conn;
        conn = tntdb::connectCached(url);
        if( elementId == UINT32_MAX ) {
            jsonResult = create_error_json( "Invalid element Id " + std::to_string(elementId) , 103 );
            return HTTP_BAD_REQUEST;
        }

        // check if element is rack
        auto rack = persist::select_asset_element_web_byId( conn, elementId );        
        if( ( ! rack.status ) || ( rack.item.type_id != persist::asset_type::RACK ) ) {
            jsonResult = create_error_json( "Specified asset element is not rack", 105 );
            return HTTP_BAD_REQUEST;
        }

        // get the rack u_size
        std::vector<device_info_t> rackv = { std::make_tuple( elementId, "", "", 0 ) };
        int freeusize = get_devices_usize(conn,rackv);
        if( ! freeusize ) {
            jsonResult = create_error_json( "Rack doesn't have u_size specified", 103 );
            return HTTP_BAD_REQUEST;
        }
        log_debug( "rack size is %i", freeusize );
            
        // get devices inside the rack
        auto devices = persist::select_asset_device_by_container(conn, elementId);
        if( ! devices.status ) {
            jsonResult = create_error_json( "Error reading rack content", 104 );
            return HTTP_BAD_REQUEST;
        }

        // substract sum( device size ) if there are some
        freeusize -= devices.item.empty() ? 0 : get_devices_usize( conn, devices.item );
        jsonResult = "{ \"id\":" + std::to_string(elementId) + ", \"freeusize\":" + std::to_string(freeusize) + " }" ;
        return HTTP_OK;
    } catch(std::exception &e) {
        jsonResult = create_error_json( e.what(), 105 );
        return HTTP_BAD_REQUEST;
    }
}

static uint32_t
s_select_outlet_count(
        tntdb::Connection &conn,
        a_elmnt_id_t id)
{
    static const char* KEY = "outlet.count";

    std::map <std::string, std::pair<std::string, bool> > res;
    int ret = persist::select_ext_attributes(conn, id, res);

    if (ret != 0 || res.count(KEY) == 0)
        return UINT32_MAX;

    std::string foo = res.at(KEY).first.c_str();
    auto dot_i = foo.find('.');
    if (dot_i != std::string::npos) {
        foo.erase(dot_i, foo.size() - dot_i);
    }

    return string_to_uint32(foo.c_str());
}

static bool
s_is_rack(
        tntdb::Connection &conn,
        a_elmnt_id_t id)
{
    auto res = persist::select_asset_element_web_byId(conn, id);
    return res.status == 1 && res.item.type_name == "rack";
}

int
rack_outlets_available(
        uint32_t elementId,
        std::map<std::string, int> &res,
        std::string &errmsg,
        int &errcode)
{
    int sum = -1;
    bool tainted = false;
    tntdb::Connection conn;

    std::function<void(const tntdb::Row &row)> cb = \
        [&conn, &sum, &tainted, &res](const tntdb::Row &row)
        {
            a_elmnt_id_t device_subtype = 0;
            row["subtype"].get(device_subtype);
            if (!persist::is_epdu(device_subtype) && !persist::is_pdu(device_subtype))
                return;

            a_elmnt_id_t device_asset_id = 0;
            row["asset_id"].get(device_asset_id);

            uint32_t foo = s_select_outlet_count(conn, device_asset_id);
            int outlet_count = foo != UINT32_MAX ? foo : -1;

            int outlet_used = persist::count_of_link_src(conn, device_asset_id);
            if (outlet_used == -1)
                outlet_count = -1;
            else
                outlet_count -= outlet_used;

            if (outlet_count < 0)
                tainted = true;
            else
                sum += outlet_count;
            res[std::to_string(device_asset_id)] = outlet_count;
        };

    try{
        conn = tntdb::connectCached(url);
        if (!s_is_rack(conn, elementId)) {
            errmsg = "Asset element with id '" + std::to_string(elementId) + "' does not exist or is not rack";
            errcode = 105;
            return HTTP_BAD_REQUEST;
        }

        persist::select_asset_device_by_container(
                conn, elementId, cb);

    } catch (std::exception &e) {
        errmsg = e.what();
        errcode = 105;
        return HTTP_BAD_REQUEST;
    }

    if (tainted || sum == -1)
        res["sum"] = -1;
    else
        res["sum"] = sum + 1;   //sum is initialized to -1

    return HTTP_OK;
}
