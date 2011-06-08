/* This file is part of Metaproxy.
   Copyright (C) 2005-2011 Index Data

Metaproxy is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

Metaproxy is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "config.hpp"
#include "filter_zoom.hpp"
#include <yaz/zoom.h>
#include <metaproxy/package.hpp>
#include <metaproxy/util.hpp>
#include "torus.hpp"

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include <yaz/oid_db.h>
#include <yaz/diagbib1.h>
#include <yaz/log.h>
#include <yaz/zgdu.h>
#include <yaz/querytowrbuf.h>

namespace mp = metaproxy_1;
namespace yf = mp::filter;

namespace metaproxy_1 {
    namespace filter {
        struct Zoom::Searchable {
            std::string database;
            std::string target;
            std::string query_encoding;
            std::string sru;
            bool piggyback;
            Searchable();
            ~Searchable();
        };
        class Zoom::Backend {
            friend class Impl;
            friend class Frontend;
            std::string zurl;
            ZOOM_connection m_connection;
            ZOOM_resultset m_resultset;
            std::string m_frontend_database;
        public:
            Backend();
            ~Backend();
            void connect(std::string zurl, int *error, const char **addinfo);
            void search_pqf(const char *pqf, Odr_int *hits,
                            int *error, const char **addinfo);
            void present(Odr_int start, Odr_int number, ZOOM_record *recs,
                         int *error, const char **addinfo);
            void set_option(const char *name, const char *value);
            int get_error(const char **addinfo);
        };
        class Zoom::Frontend {
            friend class Impl;
            Impl *m_p;
            bool m_is_virtual;
            bool m_in_use;
            yazpp_1::GDU m_init_gdu;
            BackendPtr m_backend;
            void handle_package(mp::Package &package);
            void handle_search(mp::Package &package);
            void handle_present(mp::Package &package);
            BackendPtr get_backend_from_databases(std::string &database,
                                                  int *error,
                                                  const char **addinfo);
            Z_Records *get_records(Odr_int start,
                                   Odr_int number_to_present,
                                   int *error,
                                   const char **addinfo,
                                   Odr_int *number_of_records_returned,
                                   ODR odr, BackendPtr b,
                                   Odr_oid *preferredRecordSyntax,
                                   const char *element_set_name);
        public:
            Frontend(Impl *impl);
            ~Frontend();
        };
        class Zoom::Impl {
            friend class Frontend;
        public:
            Impl();
            ~Impl();
            void process(metaproxy_1::Package & package);
            void configure(const xmlNode * ptr, bool test_only);
        private:
            FrontendPtr get_frontend(mp::Package &package);
            void release_frontend(mp::Package &package);
            void parse_torus(const xmlNode *ptr);

            std::list<Zoom::Searchable>m_searchables;

            std::map<mp::Session, FrontendPtr> m_clients;            
            boost::mutex m_mutex;
            boost::condition m_cond_session_ready;
            mp::Torus torus;
        };
    }
}

// define Pimpl wrapper forwarding to Impl
 
yf::Zoom::Zoom() : m_p(new Impl)
{
}

yf::Zoom::~Zoom()
{  // must have a destructor because of boost::scoped_ptr
}

void yf::Zoom::configure(const xmlNode *xmlnode, bool test_only)
{
    m_p->configure(xmlnode, test_only);
}

void yf::Zoom::process(mp::Package &package) const
{
    m_p->process(package);
}


// define Implementation stuff

yf::Zoom::Backend::Backend()
{
    m_connection = ZOOM_connection_create(0);
    m_resultset = 0;
}

yf::Zoom::Backend::~Backend()
{
    ZOOM_connection_destroy(m_connection);
    ZOOM_resultset_destroy(m_resultset);
}

void yf::Zoom::Backend::connect(std::string zurl,
                                int *error, const char **addinfo)
{
    ZOOM_connection_connect(m_connection, zurl.c_str(), 0);
    *error = ZOOM_connection_error(m_connection, 0, addinfo);
}

void yf::Zoom::Backend::search_pqf(const char *pqf, Odr_int *hits,
                                   int *error, const char **addinfo)
{
    m_resultset = ZOOM_connection_search_pqf(m_connection, pqf);
    *error = ZOOM_connection_error(m_connection, 0, addinfo);
    if (*error == 0)
        *hits = ZOOM_resultset_size(m_resultset);
    else
        *hits = 0;
}

void yf::Zoom::Backend::present(Odr_int start, Odr_int number,
                                ZOOM_record *recs,
                                int *error, const char **addinfo)
{
    ZOOM_resultset_records(m_resultset, recs, start, number);
    *error = ZOOM_connection_error(m_connection, 0, addinfo);
}

void yf::Zoom::Backend::set_option(const char *name, const char *value)
{
    ZOOM_connection_option_set(m_connection, name, value);
    if (m_resultset)
        ZOOM_resultset_option_set(m_resultset, name, value);
}

int yf::Zoom::Backend::get_error(const char **addinfo)
{
    return ZOOM_connection_error(m_connection, 0, addinfo);
}

yf::Zoom::Searchable::Searchable()
{
    piggyback = true;
}

yf::Zoom::Searchable::~Searchable()
{
}

yf::Zoom::Frontend::Frontend(Impl *impl) : 
    m_p(impl), m_is_virtual(false), m_in_use(true)
{
}

yf::Zoom::Frontend::~Frontend()
{
}

yf::Zoom::FrontendPtr yf::Zoom::Impl::get_frontend(mp::Package &package)
{
    boost::mutex::scoped_lock lock(m_mutex);

    std::map<mp::Session,yf::Zoom::FrontendPtr>::iterator it;
    
    while(true)
    {
        it = m_clients.find(package.session());
        if (it == m_clients.end())
            break;
        
        if (!it->second->m_in_use)
        {
            it->second->m_in_use = true;
            return it->second;
        }
        m_cond_session_ready.wait(lock);
    }
    FrontendPtr f(new Frontend(this));
    m_clients[package.session()] = f;
    f->m_in_use = true;
    return f;
}

void yf::Zoom::Impl::release_frontend(mp::Package &package)
{
    boost::mutex::scoped_lock lock(m_mutex);
    std::map<mp::Session,yf::Zoom::FrontendPtr>::iterator it;
    
    it = m_clients.find(package.session());
    if (it != m_clients.end())
    {
        if (package.session().is_closed())
        {
            m_clients.erase(it);
        }
        else
        {
            it->second->m_in_use = false;
        }
        m_cond_session_ready.notify_all();
    }
}

yf::Zoom::Impl::Impl()
{
}

yf::Zoom::Impl::~Impl()
{ 
}

void yf::Zoom::Impl::parse_torus(const xmlNode *ptr1)
{
    if (!ptr1)
        return ;
    for (ptr1 = ptr1->children; ptr1; ptr1 = ptr1->next)
    {
        if (ptr1->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((const char *) ptr1->name, "record"))
        {
            const xmlNode *ptr2 = ptr1;
            for (ptr2 = ptr2->children; ptr2; ptr2 = ptr2->next)
            {
                if (ptr2->type != XML_ELEMENT_NODE)
                    continue;
                if (!strcmp((const char *) ptr2->name, "layer"))
                {
                    Zoom::Searchable s;

                    const xmlNode *ptr3 = ptr2;
                    for (ptr3 = ptr3->children; ptr3; ptr3 = ptr3->next)
                    {
                        if (ptr3->type != XML_ELEMENT_NODE)
                            continue;
                        if (!strcmp((const char *) ptr3->name, "id"))
                        {
                            s.database = mp::xml::get_text(ptr3);
                        }
                        else if (!strcmp((const char *) ptr3->name, "zurl"))
                        {
                            s.target = mp::xml::get_text(ptr3);
                        }
                        else if (!strcmp((const char *) ptr3->name, "sru"))
                        {
                            s.sru = mp::xml::get_text(ptr3);
                        }
                        else if (!strcmp((const char *) ptr3->name,
                                         "queryEncoding"))
                        {
                            s.query_encoding = mp::xml::get_text(ptr3);
                        }
                        else if (!strcmp((const char *) ptr3->name,
                                         "piggyback"))
                        {
                            s.piggyback = mp::xml::get_bool(ptr3, true);
                        }
                    }
                    if (s.database.length() && s.target.length())
                    {
                        yaz_log(YLOG_LOG, "add db=%s target=%s", 
                                s.database.c_str(), s.target.c_str());
                        m_searchables.push_back(s);
                    }
                }
            }
        }
    }
}


void yf::Zoom::Impl::configure(const xmlNode *ptr, bool test_only)
{
    for (ptr = ptr->children; ptr; ptr = ptr->next)
    {
        if (ptr->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((const char *) ptr->name, "records"))
        {
            parse_torus(ptr);
        }
        else if (!strcmp((const char *) ptr->name, "torus"))
        {
            std::string url;
            const struct _xmlAttr *attr;
            for (attr = ptr->properties; attr; attr = attr->next)
            {
                if (!strcmp((const char *) attr->name, "url"))
                    url = mp::xml::get_text(attr->children);
                else
                    throw mp::filter::FilterException(
                        "Bad attribute " + std::string((const char *)
                                                       attr->name));
            }
            torus.read_searchables(url);
            xmlDoc *doc = torus.get_doc();
            if (doc)
            {
                xmlNode *ptr = xmlDocGetRootElement(doc);
                parse_torus(ptr);
            }
        }
        else
        {
            throw mp::filter::FilterException
                ("Bad element " 
                 + std::string((const char *) ptr->name)
                 + " in zoom filter");
        }
    }
}

yf::Zoom::BackendPtr yf::Zoom::Frontend::get_backend_from_databases(
    std::string &database, int *error, const char **addinfo)
{
    std::list<BackendPtr>::const_iterator map_it;
    if (m_backend && m_backend->m_frontend_database == database)
        return m_backend;

    std::list<Zoom::Searchable>::iterator map_s =
        m_p->m_searchables.begin();

    std::string c_db = mp::util::database_name_normalize(database);

    while (map_s != m_p->m_searchables.end())
    {
        if (c_db.compare(map_s->database) == 0)
            break;
        map_s++;
    }
    if (map_s == m_p->m_searchables.end())
    {
        *error = YAZ_BIB1_DATABASE_DOES_NOT_EXIST;
        *addinfo = database.c_str();
        BackendPtr b;
        return b;
    }

    m_backend.reset();

    BackendPtr b(new Backend);

    b->m_frontend_database = database;

    if (map_s->query_encoding.length())
        b->set_option("rpnCharset", map_s->query_encoding.c_str());

    std::string url;
    if (map_s->sru.length())
    {
        url = "http://" + map_s->target;
        b->set_option("sru", map_s->sru.c_str());
    }
    else
        url = map_s->target;

    b->connect(url, error, addinfo);
    if (*error == 0)
    {
        m_backend = b;
    }
    return b;
}

Z_Records *yf::Zoom::Frontend::get_records(Odr_int start,
                                           Odr_int number_to_present,
                                           int *error,
                                           const char **addinfo,
                                           Odr_int *number_of_records_returned,
                                           ODR odr,
                                           BackendPtr b,
                                           Odr_oid *preferredRecordSyntax,
                                           const char *element_set_name)
{
    *number_of_records_returned = 0;
    Z_Records *records = 0;

    if (start < 0 || number_to_present <= 0)
        return records;
    
    if (number_to_present > 10000)
        number_to_present = 10000;
    
    ZOOM_record *recs = (ZOOM_record *)
        odr_malloc(odr, number_to_present * sizeof(*recs));

    char oid_name_str[OID_STR_MAX];
    const char *syntax_name = 0;

    if (preferredRecordSyntax)
        syntax_name =
            yaz_oid_to_string_buf(preferredRecordSyntax, 0, oid_name_str);
    b->set_option("preferredRecordSyntax", syntax_name);
        
    b->set_option("elementSetName", element_set_name);

    b->present(start, number_to_present, recs, error, addinfo);

    Odr_int i = 0;
    if (!*error)
    {
        for (i = 0; i < number_to_present; i++)
            if (!recs[i])
                break;
    }
    if (i > 0)
    {  // only return records if no error and at least one record
        char *odr_database = odr_strdup(odr,
                                        b->m_frontend_database.c_str());
        Z_NamePlusRecordList *npl = (Z_NamePlusRecordList *)
            odr_malloc(odr, sizeof(*npl));
        *number_of_records_returned = i;
        npl->num_records = i;
        npl->records = (Z_NamePlusRecord **)
            odr_malloc(odr, i * sizeof(*npl->records));
        for (i = 0; i < number_to_present; i++)
        {
            Z_NamePlusRecord *npr = 0;
            const char *addinfo;
            int sur_error = ZOOM_record_error(recs[i], 0 /* msg */,
                                              &addinfo, 0 /* diagset */);
                
            if (sur_error)
            {
                npr = zget_surrogateDiagRec(odr, odr_database, sur_error,
                                            addinfo);
            }
            else
            {
                npr = (Z_NamePlusRecord *) odr_malloc(odr, sizeof(*npr));
                Z_External *ext =
                    (Z_External *) ZOOM_record_get(recs[i], "ext", 0);
                npr->databaseName = odr_database;
                if (ext)
                {
                    npr->which = Z_NamePlusRecord_databaseRecord;
                    npr->u.databaseRecord = ext;
                }
            }
            npl->records[i] = npr;
        }
        records = (Z_Records*) odr_malloc(odr, sizeof(*records));
        records->which = Z_Records_DBOSD;
        records->u.databaseOrSurDiagnostics = npl;
    }
    return records;
}
    

void yf::Zoom::Frontend::handle_search(mp::Package &package)
{
    Z_GDU *gdu = package.request().get();
    Z_APDU *apdu_req = gdu->u.z3950;
    Z_APDU *apdu_res = 0;
    mp::odr odr;
    Z_SearchRequest *sr = apdu_req->u.searchRequest;
    if (sr->num_databaseNames != 1)
    {
        apdu_res = odr.create_searchResponse(
            apdu_req, YAZ_BIB1_TOO_MANY_DATABASES_SPECIFIED, 0);
        package.response() = apdu_res;
        return;
    }

    int error = 0;
    const char *addinfo = 0;
    std::string db(sr->databaseNames[0]);
    BackendPtr b = get_backend_from_databases(db, &error, &addinfo);
    if (error)
    {
        apdu_res = 
            odr.create_searchResponse(
                apdu_req, error, addinfo);
        package.response() = apdu_res;
        return;
    }

    b->set_option("setname", "default");

    Odr_int hits = 0;
    Z_Query *query = sr->query;
    if (query->which == Z_Query_type_1 || query->which == Z_Query_type_101)
    {
        WRBUF w = wrbuf_alloc();
        yaz_rpnquery_to_wrbuf(w, query->u.type_1);

        b->search_pqf(wrbuf_cstr(w), &hits, &error, &addinfo);
        wrbuf_destroy(w);
    }
    else
    {
        apdu_res = 
            odr.create_searchResponse(apdu_req, YAZ_BIB1_QUERY_TYPE_UNSUPP, 0);
        package.response() = apdu_res;
        return;
    }
    
    const char *element_set_name = 0;
    Odr_int number_to_present = 0;
    if (!error)
        mp::util::piggyback_sr(sr, hits, number_to_present, &element_set_name);
    
    Odr_int number_of_records_returned = 0;
    Z_Records *records = get_records(
        0, number_to_present, &error, &addinfo,
        &number_of_records_returned, odr, b, sr->preferredRecordSyntax,
        element_set_name);
    apdu_res = odr.create_searchResponse(apdu_req, error, addinfo);
    if (records)
    {
        apdu_res->u.searchResponse->records = records;
        apdu_res->u.searchResponse->numberOfRecordsReturned =
            odr_intdup(odr, number_of_records_returned);
    }
    apdu_res->u.searchResponse->resultCount = odr_intdup(odr, hits);
    package.response() = apdu_res;
}

void yf::Zoom::Frontend::handle_present(mp::Package &package)
{
    Z_GDU *gdu = package.request().get();
    Z_APDU *apdu_req = gdu->u.z3950;
    Z_APDU *apdu_res = 0;
    Z_PresentRequest *pr = apdu_req->u.presentRequest;

    mp::odr odr;
    if (!m_backend)
    {
        package.response() = odr.create_presentResponse(
            apdu_req, YAZ_BIB1_SPECIFIED_RESULT_SET_DOES_NOT_EXIST, 0);
        return;
    }
    const char *element_set_name = 0;
    Z_RecordComposition *comp = pr->recordComposition;
    if (comp && comp->which != Z_RecordComp_simple)
    {
        package.response() = odr.create_presentResponse(
            apdu_req, 
            YAZ_BIB1_PRESENT_COMP_SPEC_PARAMETER_UNSUPP, 0);
        return;
    }
    if (comp && comp->u.simple->which == Z_ElementSetNames_generic)
        element_set_name = comp->u.simple->u.generic;
    Odr_int number_of_records_returned = 0;
    int error = 0;
    const char *addinfo = 0;
    Z_Records *records = get_records(
        *pr->resultSetStartPoint - 1, *pr->numberOfRecordsRequested,
        &error, &addinfo, &number_of_records_returned, odr, m_backend,
        pr->preferredRecordSyntax, element_set_name);

    apdu_res = odr.create_presentResponse(apdu_req, error, addinfo);
    if (records)
    {
        apdu_res->u.presentResponse->records = records;
        apdu_res->u.presentResponse->numberOfRecordsReturned =
            odr_intdup(odr, number_of_records_returned);
    }
    package.response() = apdu_res;
}

void yf::Zoom::Frontend::handle_package(mp::Package &package)
{
    Z_GDU *gdu = package.request().get();
    if (!gdu)
        ;
    else if (gdu->which == Z_GDU_Z3950)
    {
        Z_APDU *apdu_req = gdu->u.z3950;
        if (apdu_req->which == Z_APDU_initRequest)
        {
            mp::odr odr;
            package.response() = odr.create_close(
                apdu_req,
                Z_Close_protocolError,
                "double init");
        }
        else if (apdu_req->which == Z_APDU_searchRequest)
        {
            handle_search(package);
        }
        else if (apdu_req->which == Z_APDU_presentRequest)
        {
            handle_present(package);
        }
        else
        {
            mp::odr odr;
            package.response() = odr.create_close(
                apdu_req,
                Z_Close_protocolError,
                "zoom filter cannot handle this APDU");
            package.session().close();
        }
    }
    else
    {
        package.session().close();
    }
}

void yf::Zoom::Impl::process(mp::Package &package)
{
    FrontendPtr f = get_frontend(package);
    Z_GDU *gdu = package.request().get();

    if (f->m_is_virtual)
    {
        f->handle_package(package);
    }
    else if (gdu && gdu->which == Z_GDU_Z3950 && gdu->u.z3950->which ==
             Z_APDU_initRequest)
    {
        Z_InitRequest *req = gdu->u.z3950->u.initRequest;
        f->m_init_gdu = gdu;
        
        mp::odr odr;
        Z_APDU *apdu = odr.create_initResponse(gdu->u.z3950, 0, 0);
        Z_InitResponse *resp = apdu->u.initResponse;
        
        int i;
        static const int masks[] = {
            Z_Options_search,
            Z_Options_present,
            -1 
        };
        for (i = 0; masks[i] != -1; i++)
            if (ODR_MASK_GET(req->options, masks[i]))
                ODR_MASK_SET(resp->options, masks[i]);
        
        static const int versions[] = {
            Z_ProtocolVersion_1,
            Z_ProtocolVersion_2,
            Z_ProtocolVersion_3,
            -1
        };
        for (i = 0; versions[i] != -1; i++)
            if (ODR_MASK_GET(req->protocolVersion, versions[i]))
                ODR_MASK_SET(resp->protocolVersion, versions[i]);
            else
                break;
        
        *resp->preferredMessageSize = *req->preferredMessageSize;
        *resp->maximumRecordSize = *req->maximumRecordSize;
        
        package.response() = apdu;
        f->m_is_virtual = true;
    }
    else
        package.move();

    release_frontend(package);
}


static mp::filter::Base* filter_creator()
{
    return new mp::filter::Zoom;
}

extern "C" {
    struct metaproxy_1_filter_struct metaproxy_1_filter_zoom = {
        0,
        "zoom",
        filter_creator
    };
}


/*
 * Local variables:
 * c-basic-offset: 4
 * c-file-style: "Stroustrup"
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */

