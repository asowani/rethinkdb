// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/servers/server_config.hpp"

#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/servers/name_client.hpp"
#include "concurrency/cross_thread_signal.hpp"

bool convert_server_config_and_name_from_datum(
        ql::datum_t datum,
        name_string_t *name_out,
        server_id_t *server_id_out,
        std::set<name_string_t> *tags_out,
        std::string *error_out) {
    converter_from_datum_object_t converter;
    if (!converter.init(datum, error_out)) {
        return false;
    }

    ql::datum_t name_datum;
    if (!converter.get("name", &name_datum, error_out)) {
        return false;
    }
    if (!convert_name_from_datum(name_datum, "server name", name_out, error_out)) {
        *error_out = "In `name`: " + *error_out;
        return false;
    }

    ql::datum_t id_datum;
    if (!converter.get("id", &id_datum, error_out)) {
        return false;
    }
    if (!convert_uuid_from_datum(id_datum, server_id_out, error_out)) {
        *error_out = "In `id`: " + *error_out;
        return false;
    }

    ql::datum_t tags_datum;
    if (!converter.get("tags", &tags_datum, error_out)) {
        return false;
    }
    if (!convert_set_from_datum<name_string_t>(
            [] (ql::datum_t datum2, name_string_t *val2_out,
                    std::string *error2_out) {
                return convert_name_from_datum(datum2, "server tag", val2_out,
                    error2_out);
            },
            true,   /* don't complain if a tag appears twice */
            tags_datum, tags_out, error_out)) {
        *error_out = "In `tags`: " + *error_out;
        return false;
    }

    if (!converter.check_no_extra_keys(error_out)) {
        return false;
    }

    return true;
}

server_config_artificial_table_backend_t::server_config_artificial_table_backend_t(
        boost::shared_ptr< semilattice_readwrite_view_t<
            servers_semilattice_metadata_t> > _servers_sl_view,
        server_name_client_t *_name_client) :
    common_server_artificial_table_backend_t(_servers_sl_view, _name_client)
    { }

server_config_artificial_table_backend_t::~server_config_artificial_table_backend_t() {
    begin_changefeed_destruction();
}

bool server_config_artificial_table_backend_t::format_row(
        name_string_t const & server_name,
        server_id_t const & server_id,
        server_semilattice_metadata_t const & server_sl,
        UNUSED signal_t *interruptor,
        ql::datum_t *row_out,
        UNUSED std::string *error_out) {
    ql::datum_object_builder_t builder;

    builder.overwrite("name", convert_name_to_datum(server_name));
    builder.overwrite("id", convert_uuid_to_datum(server_id));
    builder.overwrite("tags", convert_set_to_datum<name_string_t>(
            &convert_name_to_datum, server_sl.tags.get_ref()));

    *row_out = std::move(builder).to_datum();

    return true;
}

bool server_config_artificial_table_backend_t::write_row(
        ql::datum_t primary_key,
        UNUSED bool pkey_was_autogenerated,
        ql::datum_t *new_value_inout,
        signal_t *interruptor,
        std::string *error_out) {
    cross_thread_signal_t interruptor2(interruptor, home_thread());
    on_thread_t thread_switcher(home_thread());
    new_mutex_in_line_t write_mutex_in_line(&write_mutex);
    wait_interruptible(write_mutex_in_line.acq_signal(), &interruptor2);
    servers_semilattice_metadata_t servers_sl = servers_sl_view->get();
    name_string_t server_name;
    server_id_t server_id;
    server_semilattice_metadata_t *server_sl;
    if (!lookup(primary_key, &servers_sl, &server_name, &server_id, &server_sl)) {
        if (new_value_inout->has()) {
            *error_out = "It's illegal to insert new rows into the "
                "`rethinkdb.server_config` artificial table.";
            return false;
        } else {
            /* The user is re-deleting an already-absent row. OK. */
            return true;
        }
    }
    if (new_value_inout->has()) {
        name_string_t new_server_name;
        server_id_t new_server_id;
        std::set<name_string_t> new_tags;
        if (!convert_server_config_and_name_from_datum(*new_value_inout,
                &new_server_name, &new_server_id, &new_tags, error_out)) {
            *error_out = "The row you're trying to put into `rethinkdb.server_config` "
                "has the wrong format. " + *error_out;
            return false;
        }
        guarantee(server_id == new_server_id, "artificial_table_t should ensure that "
            "primary key is unchanged.");
        if (new_server_name != server_name) {
            if (!name_client->rename_server(server_id, server_name, new_server_name,
                                            &interruptor2, error_out)) {
                return false;
            }
        }
        if (new_tags != server_sl->tags.get_ref()) {
            if (!name_client->retag_server(
                    server_id, server_name, new_tags, &interruptor2, error_out)) {
                return false;
            }
        }
        return true;
    } else {
        servers_sl.servers.at(server_id).mark_deleted();
        servers_sl_view->join(servers_sl);
        return true;
    }
}

