/*
 * FUNCTION:
 * Persistent Atom storage, ODBC SQL-backed.
 *
 * Atoms are saved to, and restored from, an SQL DB using the ODBC driver.
 * Atoms are identified by means of unique ID's, which are taken to
 * be the atom Handles, as maintained by the TLB. In particular, the
 * system here depends on the handles in the TLB and in the SQL DB
 * to be consistent (i.e. kept in sync).
 *
 * Copyright (c) 2008,2009,2013 Linas Vepstas <linas@linas.org>
 *
 * LICENSE:
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifdef HAVE_SQL_STORAGE

#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <thread>

#include <opencog/util/oc_assert.h>
#include <opencog/atoms/base/Atom.h>
#include <opencog/atoms/base/ClassServer.h>
#include <opencog/atoms/base/Link.h>
#include <opencog/atoms/base/Node.h>
#include <opencog/truthvalue/CountTruthValue.h>
#include <opencog/truthvalue/IndefiniteTruthValue.h>
#include <opencog/truthvalue/ProbabilisticTruthValue.h>
#include <opencog/truthvalue/SimpleTruthValue.h>
#include <opencog/truthvalue/TruthValue.h>
#include <opencog/atomspace/AtomSpace.h>
#include <opencog/atomspaceutils/TLB.h>

#include "odbcxx.h"
#include "ODBCAtomStorage.h"

using namespace opencog;

#define USE_INLINE_EDGES

/* ================================================================ */

/**
 * Utility class, hangs on to a single response to an SQL query,
 * and provides routines to parse it, i.e. walk the rows and columns,
 * converting each row into an Atom, or Edge.
 *
 * Intended to be allocated on stack, to avoid malloc overhead.
 * Methods are intended to be inlined, so as to avoid subroutine
 * call overhead.  It really *is* supposed to be a convenience wrapper. :-)
 */
class ODBCAtomStorage::Response
{
    public:
        ODBCRecordSet *rs;

        // Temporary cache of info about atom being assembled.
        UUID uuid;
        int itype;
        const char * name;
        int tv_type;
        double mean;
        double confidence;
        double count;
        const char *outlist;
        int height;

        Response()
        {
            tname = "";
            itype = 0;
            intval = 0;
        }

        void release()
        {
            if (rs) rs->release();
            rs = nullptr;
        }
        bool create_atom_column_cb(const char *colname, const char * colvalue)
        {
            // printf ("%s = %s\n", colname, colvalue);
            if (!strcmp(colname, "type"))
            {
                itype = atoi(colvalue);
            }
            else if (!strcmp(colname, "name"))
            {
                name = colvalue;
            }
            else if (!strcmp(colname, "outgoing"))
            {
                outlist = colvalue;
            }
            if (!strcmp(colname, "tv_type"))
            {
                tv_type = atoi(colvalue);
            }
            else if (!strcmp(colname, "stv_mean"))
            {
                mean = atof(colvalue);
            }
            else if (!strcmp(colname, "stv_confidence"))
            {
                confidence = atof(colvalue);
            }
            else if (!strcmp(colname, "stv_count"))
            {
                count = atof(colvalue);
            }
            else if (!strcmp(colname, "uuid"))
            {
                uuid = strtoul(colvalue, NULL, 10);
            }
            return false;
        }
        bool create_atom_cb(void)
        {
            // printf ("---- New atom found ----\n");
            rs->foreach_column(&Response::create_atom_column_cb, this);

            return false;
        }

        AtomTable *table;
        ODBCAtomStorage *store;
        bool load_all_atoms_cb(void)
        {
            // printf ("---- New atom found ----\n");
            rs->foreach_column(&Response::create_atom_column_cb, this);

            PseudoPtr p(store->makeAtom(*this, uuid));
            AtomPtr atom(get_recursive_if_not_exists(p));
            table->add(atom, true);
            return false;
        }

        // Load an atom into the atom table, but only if it's not in
        // it already.  The goal is to avoid clobbering the truth value
        // that is currently in the AtomTable.  Adding an atom to the
        // atom table that already exists causes the two TV's to be
        // merged, which is probably not what was wanted...
        bool load_if_not_exists_cb(void)
        {
            // printf ("---- New atom found ----\n");
            rs->foreach_column(&Response::create_atom_column_cb, this);

            if (nullptr == store->_tlbuf.getAtom(uuid))
            {
                PseudoPtr p(store->makeAtom(*this, uuid));
                AtomPtr atom(get_recursive_if_not_exists(p));
                Handle h = table->getHandle(atom);
                if (nullptr == h)
                {
                    store->_tlbuf.addAtom(atom, uuid);
                    table->add(atom, true);
                }
            }
            return false;
        }

        HandleSeq *hvec;
        bool fetch_incoming_set_cb(void)
        {
            // printf ("---- New atom found ----\n");
            rs->foreach_column(&Response::create_atom_column_cb, this);

            // Note, unlike the above 'load' routines, this merely fetches
            // the atoms, and returns a vector of them.  They are loaded
            // into the atomspace later, by the caller.
            PseudoPtr p(store->makeAtom(*this, uuid));
            AtomPtr atom(get_recursive_if_not_exists(p));
            hvec->emplace_back(atom->getHandle());
            return false;
        }

        // Helper function for above.  The problem is that, when
        // adding links of unknown provenance, it could happen that
        // the outgoing set of the link has not yet been loaded.  In
        // that case, we have to load the outgoing set first.
        AtomPtr get_recursive_if_not_exists(PseudoPtr p)
        {
            if (classserver().isA(p->type, NODE))
            {
                NodePtr node(createNode(p->type, p->name, p->tv));
                store->_tlbuf.addAtom(node, p->uuid);
                return node;
            }
            HandleSeq resolved_oset;
            for (UUID idu : p->oset)
            {
                Handle h = store->_tlbuf.getAtom(idu);
                if (h)
                {
                    resolved_oset.emplace_back(h);
                    continue;
                }
                PseudoPtr po(store->petAtom(idu));
                AtomPtr ra = get_recursive_if_not_exists(po);
                resolved_oset.emplace_back(ra->getHandle());
            }
            LinkPtr link(createLink(p->type, resolved_oset, p->tv));
            store->_tlbuf.addAtom(link, p->uuid);
            return link;
        }

        bool row_exists;
        bool row_exists_cb(void)
        {
            row_exists = true;
            return false;
        }

#ifndef USE_INLINE_EDGES
        // Temporary cache of info about the outgoing set.
        HandleSeq *outvec;
        Handle dst;
        int pos;

        bool create_edge_cb(void)
        {
            // printf ("---- New edge found ----\n");
            rs->foreach_column(&Response::create_edge_column_cb, this);
            int sz = outvec->size();
            if (sz <= pos) outvec->resize(pos+1);
            outvec->at(pos) = dst;
            return false;
        }
        bool create_edge_column_cb(const char *colname, const char * colvalue)
        {
            // printf ("%s = %s\n", colname, colvalue);
            if (!strcmp(colname, "dst_uuid"))
            {
                dst = Handle(strtoul(colvalue, (char **) NULL, 10));
            }
            else if (!strcmp(colname, "pos"))
            {
                pos = atoi(colvalue);
            }
            return false;
        }
#endif /* USE_INLINE_EDGES */

        // deal twith the type-to-id map
        bool type_cb(void)
        {
            rs->foreach_column(&Response::type_column_cb, this);
            store->set_typemap(itype, tname);
            return false;
        }
        const char * tname;
        bool type_column_cb(const char *colname, const char * colvalue)
        {
            if (!strcmp(colname, "type"))
            {
                itype = atoi(colvalue);
            }
            else if (!strcmp(colname, "typename"))
            {
                tname = colvalue;
            }
            return false;
        }
#ifdef OUT_OF_LINE_TVS
        // Callbacks for SimpleTruthValues
        int tvid;
        bool create_tv_cb(void)
        {
            // printf ("---- New SimpleTV found ----\n");
            rs->foreach_column(&Response::create_tv_column_cb, this);
            return false;
        }
        bool create_tv_column_cb(const char *colname, const char * colvalue)
        {
            printf ("%s = %s\n", colname, colvalue);
            if (!strcmp(colname, "mean"))
            {
                mean = atof(colvalue);
            }
            else if (!strcmp(colname, "count"))
            {
                count = atof(colvalue);
            }
            return false;
        }

#endif /* OUT_OF_LINE_TVS */

        // get generic positive integer values
        unsigned long intval;
        bool intval_cb(void)
        {
            rs->foreach_column(&Response::intval_column_cb, this);
            return false;
        }
        bool intval_column_cb(const char *colname, const char * colvalue)
        {
            // we're not going to bother to check the column name ...
            intval = strtoul(colvalue, NULL, 10);
            return false;
        }

        // Get all handles in the database.
        std::set<UUID> *id_set;
        bool note_id_cb(void)
        {
            rs->foreach_column(&Response::note_id_column_cb, this);
            return false;
        }
        bool note_id_column_cb(const char *colname, const char * colvalue)
        {
            // we're not going to bother to check the column name ...
            UUID id = strtoul(colvalue, NULL, 10);
            id_set->insert(id);
            return false;
        }
};

/* ================================================================ */
/// XXX TODO Make the connection pointer scoped.
/// That is, we should define a ConnPtr class here, and it's destructor
/// should do the conn_pool.push(). Doing this can help avoid mem leaks,
/// e.g. failure to put because of a throw.  I'm just kind of lazy now,
/// and the code below works ... so maybe it shouldn't be messed with.
///
/// XXX Should do the same for Response rp.rs->release() to auto-release.

/// Get an ODBC connection
ODBCConnection* ODBCAtomStorage::get_conn()
{
    return conn_pool.pop();
}

/// Put an ODBC connection back into the pool.
void ODBCAtomStorage::put_conn(ODBCConnection* db_conn)
{
    conn_pool.push(db_conn);
}

/* ================================================================ */

bool ODBCAtomStorage::idExists(const char * buff)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.row_exists = false;
    rp.rs = db_conn->exec(buff);
    rp.rs->foreach_row(&Response::row_exists_cb, &rp);
    rp.release();
    put_conn(db_conn);
    return rp.row_exists;
}

/* ================================================================ */
#define BUFSZ 250

#ifndef USE_INLINE_EDGES
/**
 * Callback class, whose method is invoked on each outgoing edge.
 * The callback constructs an SQL query to store the edge.
 */
class ODBCAtomStorage::Outgoing
{
    private:
        ODBCConnection *db_conn;
        unsigned int pos;
        Handle src_handle;
    public:
        Outgoing (ODBCConnection *c, Handle h)
        {
            db_conn = c;
            src_handle = h;
            pos = 0;
        }
        bool each_handle (Handle h)
        {
            char buff[BUFSZ];
            UUID src_uuid = _tlbuf.addAtom(src_handle, TLB::INVALID_UUID);
            UUID dst_uuid = _tlbuf.addAtom(h, TLB::INVALID_UUID);
            snprintf(buff, BUFSZ, "INSERT  INTO Edges "
                    "(src_uuid, dst_uuid, pos) VALUES (%lu, %lu, %u);",
                    src_uuid, dst_uuid, pos);

            Response rp;
            rp.rs = db_conn->exec(buff);
            rp.release();
            pos ++;
            return false;
        }
};

/**
 * Store the outgoing set of the atom.
 * Handle h must be the handle for the atom; its passed as an arg to
 * avoid having to look it up.
 */
void ODBCAtomStorage::storeOutgoing(AtomPtr atom, Handle h)
{
    Outgoing out(db_conn, h);

    foreach_outgoing_handle(h, &Outgoing::each_handle, &out);
}

#endif /* USE_INLINE_EDGES */

/* ================================================================ */
// Constructors

void ODBCAtomStorage::init(const char * dbname,
                       const char * username,
                       const char * authentication)
{
    // Create six, by default ... maybe make more?
    // There should probably be a few more here, than the number of
    // startWriterThread() calls below.
#define DEFAULT_NUM_CONNS 6
    for (int i=0; i<DEFAULT_NUM_CONNS; i++)
    {
        ODBCConnection* db_conn = new ODBCConnection(dbname, username, authentication);
        conn_pool.push(db_conn);
    }
    type_map_was_loaded = false;
    max_height = 0;

    for (int i=0; i< TYPEMAP_SZ; i++)
    {
        db_typename[i] = NULL;
    }

    local_id_cache_is_inited = false;
    table_cache_is_inited = false;
    if (!connected()) return;

    reserve();
}

ODBCAtomStorage::ODBCAtomStorage(const char * dbname,
                         const char * username,
                         const char * authentication)
    : _write_queue(this, &ODBCAtomStorage::vdo_store_atom)
{
    init(dbname, username, authentication);
}

ODBCAtomStorage::ODBCAtomStorage(const std::string& dbname,
                         const std::string& username,
                         const std::string& authentication)
    : _write_queue(this, &ODBCAtomStorage::vdo_store_atom)
{
    init(dbname.c_str(), username.c_str(), authentication.c_str());
}

ODBCAtomStorage::~ODBCAtomStorage()
{
    if (connected())
        setMaxHeight(getMaxObservedHeight());

    while (not conn_pool.is_empty())
    {
        ODBCConnection* db_conn = conn_pool.pop();
        delete db_conn;
    }

    for (int i=0; i<TYPEMAP_SZ; i++)
    {
        if (db_typename[i]) free(db_typename[i]);
    }
}

/**
 * connected -- return true if a successful connection to the
 * database exists; else return false.  Note that this may block,
 * if all database connections are in use...
 */
bool ODBCAtomStorage::connected(void)
{
    ODBCConnection* db_conn = get_conn();
    bool have_connection = db_conn->connected();
    put_conn(db_conn);
    return have_connection;
}

void ODBCAtomStorage::registerWith(AtomSpace* as)
{
	_tlbuf.set_resolver(&as->get_atomtable());
}

void ODBCAtomStorage::unregisterWith(AtomSpace* as)
{
	_tlbuf.clear_resolver(&as->get_atomtable());
}

/* ================================================================== */
/* AtomTable UUID stuff */

void ODBCAtomStorage::store_atomtable_id(const AtomTable& at)
{
    UUID tab_id = at.get_uuid();
    if (table_id_cache.count(tab_id)) return;

    table_id_cache.insert(tab_id);

    // Get the parent table as well.
    UUID parent_id = 1;
    AtomTable *env = at.get_environ();
    if (env)
    {
        parent_id = env->get_uuid();
        store_atomtable_id(*env);
    }

    char buff[BUFSZ];
    snprintf(buff, BUFSZ,
        "INSERT INTO Spaces (space, parent) VALUES (%ld, %ld);",
        tab_id, parent_id);

    std::unique_lock<std::mutex> lock(table_cache_mutex);
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.rs = db_conn->exec(buff);
    rp.release();
    put_conn(db_conn);
}


/* ================================================================ */

#define STMT(colname,val) { \
    if (update) { \
        if (notfirst) { cols += ", "; } else notfirst = 1; \
        cols += colname; \
        cols += " = "; \
        cols += val; \
    } else { \
        if (notfirst) { cols += ", "; vals += ", "; } else notfirst = 1; \
        cols += colname; \
        vals += val; \
    } \
}

#define STMTI(colname,ival) { \
    char buff[BUFSZ]; \
    snprintf(buff, BUFSZ, "%u", ival); \
    STMT(colname, buff); \
}

#define STMTF(colname,fval) { \
    char buff[BUFSZ]; \
    snprintf(buff, BUFSZ, "%12.8g", fval); \
    STMT(colname, buff); \
}

/* ================================================================ */

#ifdef OUT_OF_LINE_TVS
/**
 * Return true if the indicated handle exists in the storage.
 */
bool ODBCAtomStorage::tvExists(int tvid)
{
    char buff[BUFSZ];
    snprintf(buff, BUFSZ, "SELECT tvid FROM SimpleTVs WHERE tvid = %u;", tvid);
    return idExists(buff);
}

/**
 * Store the truthvalue of the atom.
 * Handle h must be the handle for the atom; its passed as an arg to
 * avoid having to look it up.
 */
int ODBCAtomStorage::storeTruthValue(AtomPtr atom, Handle h)
{
    int notfirst = 0;
    std::string cols;
    std::string vals;
    std::string coda;

    const TruthValue &tv = atom->getTruthValue();

    const SimpleTruthValue *stv = dynamic_cast<const SimpleTruthValue *>(&tv);
    if (NULL == stv)
    {
        fprintf(stderr, "Error: non-simple truth values are not handled\n");
        return 0;
    }

    int tvid = TVID(tv);

    // If its a stock truth value, there is nothing to do.
    if (tvid <= 4) return tvid;

    // Use the TLB Handle as the UUID.
    char tvidbuff[BUFSZ];
    snprintf(tvidbuff, BUFSZ, "%u", tvid);

    bool update = tvExists(tvid);
    if (update)
    {
        cols = "UPDATE SimpleTVs SET ";
        vals = "";
        coda = " WHERE tvid = ";
        coda += tvidbuff;
        coda += ";";
    }
    else
    {
        cols = "INSERT INTO SimpleTVs (";
        vals = ") VALUES (";
        coda = ");";
        STMT("tvid", tvidbuff);
    }

    STMTF("mean", tv.getMean());
    STMTF("count", tv.getCount());

    std::string qry = cols + vals + coda;
    Response rp;
    rp.rs = db_conn->exec(qry.c_str());
    rp.release();

    return tvid;
}

/**
 * Return a new, unique ID for every truth value
 */
int ODBCAtomStorage::TVID(const TruthValue &tv)
{
    if (tv == TruthValue::NULL_TV()) return 0;
    if (tv == TruthValue::TRIVIAL_TV()) return 1;
    if (tv == TruthValue::FALSE_TV()) return 2;
    if (tv == TruthValue::TRUE_TV()) return 3;
    if (tv == TruthValue::DEFAULT_TV()) return 4;

    Response rp;
    rp.rs = db_conn->exec("SELECT NEXTVAL('tvid_seq');");
    rp.rs->foreach_row(&Response::tvid_seq_cb, &rp);
    rp.release();
    return rp.tvid;
}

TruthValue* ODBCAtomStorage::getTV(int tvid)
{
    if (0 == tvid) return (TruthValue *) & TruthValue::NULL_TV();
    if (1 == tvid) return (TruthValue *) & TruthValue::DEFAULT_TV();
    if (2 == tvid) return (TruthValue *) & TruthValue::FALSE_TV();
    if (3 == tvid) return (TruthValue *) & TruthValue::TRUE_TV();
    if (4 == tvid) return (TruthValue *) & TruthValue::TRIVIAL_TV();

    char buff[BUFSZ];
    snprintf(buff, BUFSZ, "SELECT * FROM SimpleTVs WHERE tvid = %u;", tvid);

    Response rp;
    rp.rs = db_conn->exec(buff);
    rp.rs->foreach_row(&Response::create_tv_cb, &rp);
    rp.release();

    SimpleTruthValue *stv = new SimpleTruthValue(rp.mean, rp.confidence);
    return stv;
}

#endif /* OUT_OF_LINE_TVS */

/* ================================================================== */

/**
 * Return largest distance from this atom to any node under it.
 * Nodes have a height of 0, by definition.  Links that contain only
 * nodes in their outgoing set have a height of 1, by definition.
 * The height of a link is, by definition, one more than the height
 * of the tallest atom in its outgoing set.
 * @note This can conversely be viewed as the depth of a tree.
 */
int ODBCAtomStorage::get_height(AtomPtr atom)
{
    LinkPtr l(LinkCast(atom));
    if (NULL == l) return 0;

    int maxd = 0;
    int arity = l->getArity();

    const HandleSeq& out = l->getOutgoingSet();
    for (int i=0; i<arity; i++)
    {
        Handle h = out[i];
        int d = get_height(h);
        if (maxd < d) maxd = d;
    }
    return maxd +1;
}

/* ================================================================ */

std::string ODBCAtomStorage::oset_to_string(const HandleSeq& out,
                                        int arity)
{
    std::string str;
    str += "\'{";
    for (int i=0; i<arity; i++)
    {
        const Handle& h = out[i];
        UUID uuid = _tlbuf.addAtom(h, TLB::INVALID_UUID);
        if (i != 0) str += ", ";
        str += std::to_string(uuid);
    }
    str += "}\'";
    return str;
}

/* ================================================================ */

/// Drain the pending store queue.
/// Caution: this is slightly racy; a writer could still be busy
/// even though this returns. (There's a window in writeLoop, between
/// the dequeue, and the busy_writer increment. I guess we should fix
/// this...
void ODBCAtomStorage::flushStoreQueue()
{
    _write_queue.flush_queue();
}

/* ================================================================ */
/**
 * Recursively store the indicated atom, and all that it points to.
 * Store its truth values too. The recursive store is unconditional;
 * its assumed that all sorts of underlying truuth values have changed,
 * so that the whole thing needs to be stored.
 *
 * By default, the actual store is done asynchronously (in a different
 * thread); this routine merely queues up the atom. If the synchronous
 * flag is set, then the store is done in this thread.
 */
void ODBCAtomStorage::storeAtom(const AtomPtr& atom, bool synchronous)
{
    get_ids();

    // If a synchronous store, avoid the queues entirely.
    if (synchronous)
    {
        do_store_atom(atom);
        return;
    }
    _write_queue.enqueue(atom);
}

/**
 * Synchronously store a single atom. That is, the actual store is done
 * in the calling thread.
 * Returns the height of the atom.
 */
int ODBCAtomStorage::do_store_atom(AtomPtr atom)
{
    LinkPtr l(LinkCast(atom));
    if (NULL == l)
    {
        do_store_single_atom(atom, 0);
        return 0;
    }

    int lheight = 0;
    int arity = l->getArity();
    const HandleSeq& out = l->getOutgoingSet();
    for (int i=0; i<arity; i++)
    {
        // Recurse.
        int heig = do_store_atom(out[i]);
        if (lheight < heig) lheight = heig;
    }

    // Height of this link is, by definition, one more than tallest
    // atom in outgoing set.
    lheight ++;
    do_store_single_atom(atom, lheight);
    return lheight;
}

void ODBCAtomStorage::vdo_store_atom(const AtomPtr& atom)
{
    do_store_atom(atom);
}

/* ================================================================ */
/**
 * Store the single, indicated atom.
 * Store its truth values too.
 * The store is performed synchnously (in the calling thread).
 */
void ODBCAtomStorage::storeSingleAtom(AtomPtr atom)
{
    get_ids();
    int height = get_height(atom);
    do_store_single_atom(atom, height);
}

void ODBCAtomStorage::do_store_single_atom(AtomPtr atom, int aheight)
{
    setup_typemap();

    int notfirst = 0;
    std::string cols;
    std::string vals;
    std::string coda;

    // Use the TLB Handle as the UUID.
    Handle h(atom->getHandle());
    UUID uuid = _tlbuf.addAtom(h, TLB::INVALID_UUID);

    std::string uuidbuff = std::to_string(uuid);

    std::unique_lock<std::mutex> lck = maybe_create_id(uuid);
    bool update = not lck.owns_lock();
    if (update)
    {
        cols = "UPDATE Atoms SET ";
        vals = "";
        coda = " WHERE uuid = ";
        coda += uuidbuff;
        coda += ";";
    }
    else
    {
        cols = "INSERT INTO Atoms (";
        vals = ") VALUES (";
        coda = ");";

        STMT("uuid", uuidbuff);
    }

    // Store the atom type and node name only if storing for the
    // first time ever. Once an atom is in an atom table, it's
    // name can type cannot be changed. Only its truth value can
    // change.
    if (false == update)
    {
        // Store the atomspace UUID
        AtomTable * at = getAtomTable(atom);
        // We allow storage of atoms that don't belong to an atomspace.
        if (at) uuidbuff = std::to_string(at->get_uuid());
        else uuidbuff = "0";
        STMT("space", uuidbuff);

        // Store the atom UUID
        Type t = atom->getType();
        int dbtype = storing_typemap[t];
        STMTI("type", dbtype);

        // Store the node name, if its a node
        NodePtr n(NodeCast(atom));
        if (n)
        {
            // Use postgres $-quoting to make unicode strings
            // easier to deal with.
            std::string qname = " $ocp$";
            qname += n->getName();
            qname += "$ocp$ ";

            // The Atoms table has a UNIQUE constraint on the
            // node name.  If a node name is too long, a postgres
            // error is generated:
            // ERROR: index row size 4440 exceeds maximum 2712
            // for index "atoms_type_name_key"
            // There's not much that can be done about this, without
            // a redesign of the table format, in some way. Maybe
            // we could hash the long node names, store the hash,
            // and make sure that is unique.
            if (2700 < qname.size())
            {
                throw RuntimeException(TRACE_INFO,
                    "Error: do_store_single_atom: Maxiumum Node name size is 2700.\n");
            }
            STMT("name", qname);

            // Nodes have a height of zero by definition.
            STMTI("height", 0);
        }
        else
        {
            if (max_height < aheight) max_height = aheight;
            STMTI("height", aheight);

#ifdef USE_INLINE_EDGES
            LinkPtr l(LinkCast(atom));
            if (l)
            {
                int arity = l->getArity();

                // The Atoms table has a UNIQUE constraint on the
                // outgoing set.  If a link is too large, a postgres
                // error is generated:
                // ERROR: index row size 4440 exceeds maximum 2712
                // for index "atoms_type_outgoing_key"
                // The simplest solution that I see requires a database
                // redesign.  One could hash together the UUID's in the
                // outgoing set, and then force a unique constraint on
                // the hash.
                if (330 < arity)
                {
                    throw RuntimeException(TRACE_INFO,
                        "Error: do_store_single_atom: Maxiumum Link size is 330.\n");
                }

                if (arity)
                {
                    cols += ", outgoing";
                    vals += ", ";
                    vals += oset_to_string(l->getOutgoingSet(), arity);
                }
            }
#endif /* USE_INLINE_EDGES */
        }
    }

    // Store the truth value
    TruthValuePtr tv(atom->getTruthValue());
    TruthValueType tvt = NULL_TRUTH_VALUE;
    if (tv) tvt = tv->getType();
    STMTI("tv_type", tvt);

    switch (tvt)
    {
        case NULL_TRUTH_VALUE:
            break;
        case SIMPLE_TRUTH_VALUE:
        case COUNT_TRUTH_VALUE:
        case PROBABILISTIC_TRUTH_VALUE:
            STMTF("stv_mean", tv->getMean());
            STMTF("stv_confidence", tv->getConfidence());
            STMTF("stv_count", tv->getCount());
            break;
        case INDEFINITE_TRUTH_VALUE:
        {
            IndefiniteTruthValuePtr itv = std::static_pointer_cast<const IndefiniteTruthValue>(tv);
            STMTF("stv_mean", itv->getL());
            STMTF("stv_count", itv->getU());
            STMTF("stv_confidence", itv->getConfidenceLevel());
            break;
        }
        default:
            throw RuntimeException(TRACE_INFO,
                "Error: store_single: Unknown truth value type\n");
    }

    // We may have to store the atom table UUID and try again...
    // We waste CPU cycles to store the atomtable, only if it failed.
    bool try_again = false;
    std::string qry = cols + vals + coda;
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.rs = db_conn->exec(qry.c_str());
    if (NULL == rp.rs) try_again = true;
    rp.release();

    if (try_again)
    {
        AtomTable *at = getAtomTable(atom);
        if (at) store_atomtable_id(*at);
        rp.rs = db_conn->exec(qry.c_str());
        rp.release();
    }
    put_conn(db_conn);

#ifndef USE_INLINE_EDGES
    // Store the outgoing handles only if we are storing for the first
    // time, otherwise do nothing. The semantics is that, once the
    // outgoing set has been determined, it cannot be changed.
    if (false == update)
    {
        storeOutgoing(atom);
    }
#endif /* USE_INLINE_EDGES */

    // Make note of the fact that this atom has been stored.
    add_id_to_cache(uuid);
}

/* ================================================================ */
/**
 * Store the concordance of type names to type values.
 *
 * The concordance is used to match up the type id's stored in
 * the SQL database, against those currently in use in the current
 * version of the opencog server. The basic problem is that types
 * can be dynamic in OpenCog -- different versions will have
 * different types, and will assign different type numbers to some
 * given type name. To overcome this, the SQL database stores all
 * atoms according to the type *name* -- although, to save space, it
 * actually stored type ids; however, the SQL type-name-to-type-id
 * mapping can be completely different than the OpenCog type-name
 * to type-id mapping. Thus, tables to convert the one to the other
 * id are needed.
 *
 * Given an opencog type t, the storing_typemap[t] will contain the
 * sqlid for the named type. The storing_typemap[t] will *always*
 * contain a valid value.
 *
 * Given an SQL type sq, the loading_typemap[sq] will contain the
 * opencog type t for the named type, or NOTYPE if this version of
 * opencog does not have this kind of atom.
 *
 * The typemaps must be constructed before any saving or loading of
 * atoms can happen. The typemaps will be a superset (union) of the
 * types used by OpenCog, and stored in the SQL table.
 */
void ODBCAtomStorage::setup_typemap(void)
{
    /* Only need to set up the typemap once. */
    if (type_map_was_loaded) return;
    type_map_was_loaded = true;

    // If we are here, we need to reconcile the types currently in
    // use, with a possibly pre-existing typemap. New types must be
    // stored.  So we start by loading a map from SQL (if its there).
    //
    // Be careful to initialize the typemap with invalid types,
    // in case there are unexpected holes in the map!
    for (int i=0; i< TYPEMAP_SZ; i++)
    {
        loading_typemap[i] = NOTYPE;
        storing_typemap[i] = -1;
        db_typename[i] = NULL;
    }

    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.rs = db_conn->exec("SELECT * FROM TypeCodes;");
    rp.store = this;
    rp.rs->foreach_row(&Response::type_cb, &rp);
    rp.release();

    unsigned int numberOfTypes = classserver().getNumberOfClasses();
    for (Type t=0; t<numberOfTypes; t++)
    {
        int sqid = storing_typemap[t];
        /* If this typename is not yet known, record it */
        if (-1 == sqid)
        {
            const char * tname = classserver().getTypeName(t).c_str();

            // Let the sql id be the same as the current type number,
            // unless this sql number is already in use, in which case
            // we need to find another, unused one.  Its in use if we
            // have a string name associated to it.
            sqid = t;

            if ((db_typename[sqid] != NULL) &&
                (loading_typemap[sqid] != t))
            {
                // Find some (any) unused type index to use in the
                // sql table. Use the lowest unused value that we
                // can find.
                for (sqid = 0; sqid<TYPEMAP_SZ; sqid++)
                {
                    if (NULL == db_typename[sqid]) break;
                }

                if (TYPEMAP_SZ <= sqid)
                {
                    put_conn(db_conn);
                    fprintf(stderr, "Fatal Error: type table overflow!\n");
                    abort();
                }
            }

            char buff[BUFSZ];
            snprintf(buff, BUFSZ,
                     "INSERT INTO TypeCodes (type, typename) "
                     "VALUES (%d, \'%s\');",
                     sqid, tname);
            rp.rs = db_conn->exec(buff);
            rp.release();
            set_typemap(sqid, tname);
        }
    }
    put_conn(db_conn);
}

void ODBCAtomStorage::set_typemap(int dbval, const char * tname)
{
    Type realtype = classserver().getType(tname);
    loading_typemap[dbval] = realtype;
    storing_typemap[realtype] = dbval;
    if (db_typename[dbval] != NULL) free (db_typename[dbval]);
    db_typename[dbval] = strdup(tname);
}

/* ================================================================ */
/**
 * Return true if the indicated handle exists in the storage.
 * Thread-safe.
 */
bool ODBCAtomStorage::atomExists(const Handle& h)
{
    UUID uuid = _tlbuf.addAtom(h, TLB::INVALID_UUID);
#ifdef ASK_SQL_SERVER
    char buff[BUFSZ];
    snprintf(buff, BUFSZ, "SELECT uuid FROM Atoms WHERE uuid = %lu;", uuid);
    return idExists(buff);
#else
    std::unique_lock<std::mutex> lock(id_cache_mutex);
    // look at the local cache of id's to see if the atom is in storage or not.
    return local_id_cache.count(uuid);
#endif
}

/**
 * Add a single UUID to the ID cache. Thread-safe.
 * This also unlocks the id-creation lock, if it was being held.
 */
void ODBCAtomStorage::add_id_to_cache(UUID uuid)
{
    std::unique_lock<std::mutex> lock(id_cache_mutex);
    local_id_cache.insert(uuid);

    // If we were previously making this ID, then we are done.
    // The other half of this is in maybe_create_id() below.
    if (0 < id_create_cache.count(uuid))
    {
        id_create_cache.erase(uuid);
    }
}

/**
 * This returns a lock that is either locked, or not, depending on
 * whether we think that the database already knows about this UUID,
 * or not.  We do this because we need to use an SQL INSERT instead
 * of an SQL UPDATE when putting a given atom in the database the first
 * time ever.  Since SQL INSERT can be used once and only once, we have
 * to avoid the case of two threads, each trying to perform an INSERT
 * in the same ID. We do this by taking the id_create_mutex, so that
 * only one writer ever gets told that its a new ID.
 */
std::unique_lock<std::mutex> ODBCAtomStorage::maybe_create_id(UUID uuid)
{
    std::unique_lock<std::mutex> create_lock(id_create_mutex);
    std::unique_lock<std::mutex> cache_lock(id_cache_mutex);
    // Look at the local cache of id's to see if the atom is in storage or not.
    if (0 < local_id_cache.count(uuid))
        return std::unique_lock<std::mutex>();

    // Is some other thread in the process of adding this ID?
    if (0 < id_create_cache.count(uuid))
    {
        cache_lock.unlock();
        while (true)
        {
            // If we are here, some other thread is making this UUID,
            // and so we need to wait till they're done. Wait by stalling
            // on the creation lock.
            std::unique_lock<std::mutex> local_create_lock(id_create_mutex);
            // If we are here, then someone finished creating some UUID.
            // Was it our ID? If so, we are done; if not, wait some more.
            cache_lock.lock();
            if (0 == id_create_cache.count(uuid))
            {
                OC_ASSERT(0 < local_id_cache.count(uuid),
                    "Atom for UUID was not created!");
                return std::unique_lock<std::mutex>();
            }
            cache_lock.unlock();
        }
    }

    // If we are here, then no one has attempted to make this UUID before.
    // Grab the maker lock, and make the damned thing already.
    id_create_cache.insert(uuid);
    return create_lock;
}

/**
 * Build up a client-side cache of all atom id's in storage
 */
void ODBCAtomStorage::get_ids(void)
{
    std::unique_lock<std::mutex> lock(id_cache_mutex);

    if (local_id_cache_is_inited) return;
    local_id_cache_is_inited = true;

    local_id_cache.clear();
    ODBCConnection* db_conn = get_conn();

    // It appears that, when the select statment returns more than
    // about a 100K to a million atoms or so, some sort of heap
    // corruption occurs in the odbc code, causing future mallocs
    // to fail. So limit the number of records processed in one go.
    // It also appears that asking for lots of records increases
    // the memory fragmentation (and/or there's a memory leak in odbc??)
#define USTEP 12003
    unsigned long rec;
    unsigned long max_nrec = getMaxObservedUUID();
    for (rec = 0; rec <= max_nrec; rec += USTEP)
    {
        char buff[BUFSZ];
        snprintf(buff, BUFSZ, "SELECT uuid FROM Atoms WHERE "
                "uuid > %lu AND uuid <= %lu;",
                 rec, rec+USTEP);

        Response rp;
        rp.id_set = &local_id_cache;
        rp.rs = db_conn->exec(buff);
        rp.rs->foreach_row(&Response::note_id_cb, &rp);
        rp.release();
    }
    put_conn(db_conn);
}

/* ================================================================ */

#ifndef USE_INLINE_EDGES
void ODBCAtomStorage::getOutgoing(HandleSeq &outv, Handle h)
{
    char buff[BUFSZ];
    UUID uuid = _tlbuf.addAtom(h, TLB::INVALID_UUID);
    snprintf(buff, BUFSZ, "SELECT * FROM Edges WHERE src_uuid = %lu;", uuid);

    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.rs = db_conn->exec(buff);
    rp.outvec = &outv;
    rp.rs->foreach_row(&Response::create_edge_cb, &rp);
    rp.release();
    put_conn(db_conn);
}
#endif /* USE_INLINE_EDGES */

/* ================================================================ */

/* One-size-fits-all atom fetcher */
ODBCAtomStorage::PseudoPtr ODBCAtomStorage::getAtom(const char * query, int height)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.uuid = _tlbuf.INVALID_UUID;
    rp.rs = db_conn->exec(query);
    rp.rs->foreach_row(&Response::create_atom_cb, &rp);

    // Did we actually find anything?
    // DO NOT USE IsInvalidHandle() HERE! It won't work, duhh!
    if (rp.uuid == _tlbuf.INVALID_UUID)
    {
        rp.release();
        put_conn(db_conn);
        return NULL;
    }

    rp.height = height;
    PseudoPtr atom(makeAtom(rp, rp.uuid));
    rp.release();
    put_conn(db_conn);
    return atom;
}

ODBCAtomStorage::PseudoPtr ODBCAtomStorage::petAtom(UUID uuid)
{
    setup_typemap();
    char buff[BUFSZ];
    snprintf(buff, BUFSZ, "SELECT * FROM Atoms WHERE uuid = %lu;", uuid);

    return getAtom(buff, -1);
}


/**
 * Retreive the entire incoming set of the indicated atom.
 */
HandleSeq ODBCAtomStorage::getIncomingSet(const Handle& h)
{
    HandleSeq iset;

    setup_typemap();

    UUID uuid = _tlbuf.addAtom(h, TLB::INVALID_UUID);
    char buff[BUFSZ];
    snprintf(buff, BUFSZ,
        "SELECT * FROM Atoms WHERE outgoing @> ARRAY[CAST(%lu AS BIGINT)];",
        uuid);

    // Note: "select * from atoms where outgoing@>array[556];" will return
    // all links with atom 556 in the outgoing set -- i.e. the incoming set of 556.
    // Could also use && here instead of @> Don't know if one is faster or not.
    // The cast to BIGINT is needed, as otherwise on gets
    // ERROR:  operator does not exist: bigint[] @> integer[]

    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.store = this;
    rp.height = -1;
    rp.hvec = &iset;
    rp.rs = db_conn->exec(buff);
    rp.rs->foreach_row(&Response::fetch_incoming_set_cb, &rp);
    rp.release();
    put_conn(db_conn);

    return iset;
}

/**
 * Fetch Node from database, with the indicated type and name.
 * If there is no such node, NULL is returned.
 * More properly speaking, the point of this routine is really
 * to fetch the associated TruthValue for this node.
 *
 * This method does *not* register the atom with any atomtable/atomspace
 */
Handle ODBCAtomStorage::getNode(Type t, const char * str)
{
    setup_typemap();
    char buff[40*BUFSZ];

    // Use postgres $-quoting to make unicode strings easier to deal with.
    int nc = snprintf(buff, 4*BUFSZ, "SELECT * FROM Atoms WHERE "
        "type = %hu AND name = $ocp$%s$ocp$ ;", storing_typemap[t], str);

    if (40*BUFSZ-1 <= nc)
    {
        fprintf(stderr, "Error: ODBCAtomStorage::getNode: buffer overflow!\n");
        buff[40*BUFSZ-1] = 0x0;
        fprintf(stderr, "\tnc=%d buffer=>>%s<<\n", nc, buff);
        return Handle();
    }

    PseudoPtr p(getAtom(buff, 0));
    if (NULL == p) return Handle();

    NodePtr node = createNode(t, str, p->tv);
    _tlbuf.addAtom(node, p->uuid);
    return node->getHandle();
}

/**
 * Fetch Link from database, with the indicated type and outgoing set.
 * If there is no such link, NULL is returned.
 * More properly speaking, the point of this routine is really
 * to fetch the associated TruthValue for this link.
 *
 * This method does *not* register the atom with any atomtable/atomspace
 */
Handle ODBCAtomStorage::getLink(Handle& h)
{
    Type t = h->getType();
    const HandleSeq& oset = h->getOutgoingSet();
    setup_typemap();

    char buff[BUFSZ];
    snprintf(buff, BUFSZ,
        "SELECT * FROM Atoms WHERE type = %hu AND outgoing = ",
        storing_typemap[t]);

    std::string ostr = buff;
    ostr += oset_to_string(oset, oset.size());
    ostr += ";";

    PseudoPtr p = getAtom(ostr.c_str(), 1);
    if (NULL == p) return Handle();

    h->setTruthValue(p->tv);
    _tlbuf.addAtom(h, p->uuid);
    return h;
}

/**
 * Instantiate a new atom, from the response buffer contents
 */
ODBCAtomStorage::PseudoPtr ODBCAtomStorage::makeAtom(Response &rp, UUID uuid)
{
    // Now that we know everything about an atom, actually construct one.
    Type realtype = loading_typemap[rp.itype];

    if (NOTYPE == realtype)
    {
        throw RuntimeException(TRACE_INFO,
            "Fatal Error: OpenCog does not have a type called %s\n",
            db_typename[rp.itype]);
        return NULL;
    }

    PseudoPtr atom(createPseudo());

    // All height zero atoms are nodes,
    // All positive height atoms are links.
    // A negative height is "unknown" and must be checked.
    if ((0 == rp.height) or
        ((-1 == rp.height) and classserver().isA(realtype, NODE)))
    {
        atom->name = rp.name;
    }
    else
    {
        char *p = (char *) rp.outlist;
        while (p)
        {
            // Break if there are no more atoms in the outgoing set
            // or if the outgoing set is empty in the first place.
            if (*p == '}' or *p == '\0') break;
            UUID out(strtoul(p+1, &p, 10));
            atom->oset.emplace_back(out);
        }
    }

    // Give the atom the correct UUID. The AtomTable will need this.
    atom->type = realtype;
    atom->uuid = uuid;

    // Now get the truth value
    switch (rp.tv_type)
    {
        case NULL_TRUTH_VALUE:
            break;

        case SIMPLE_TRUTH_VALUE:
        {
            TruthValuePtr stv(SimpleTruthValue::createTV(rp.mean, rp.confidence));
            atom->tv = stv;
            break;
        }
        case COUNT_TRUTH_VALUE:
        {
            TruthValuePtr ctv(CountTruthValue::createTV(rp.mean, rp.confidence, rp.count));
            atom->tv = ctv;
            break;
        }
        case INDEFINITE_TRUTH_VALUE:
        {
            TruthValuePtr itv(IndefiniteTruthValue::createTV(rp.mean, rp.count, rp.confidence));
            atom->tv = itv;
            break;
        }
        case PROBABILISTIC_TRUTH_VALUE:
        {
            TruthValuePtr ptv(ProbabilisticTruthValue::createTV(rp.mean, rp.confidence, rp.count));
            atom->tv = ptv;
            break;
        }
        default:
            throw RuntimeException(TRACE_INFO,
                "Error: makeAtom: Unknown truth value type\n");
    }

    load_count ++;
    if (load_count%10000 == 0)
    {
        fprintf(stderr, "\tLoaded %lu atoms.\n", (unsigned long) load_count);
    }

    add_id_to_cache(uuid);
    return atom;
}

/* ================================================================ */

void ODBCAtomStorage::load(AtomTable &table)
{
    unsigned long max_nrec = getMaxObservedUUID();
    _tlbuf.reserve_upto(max_nrec);
    fprintf(stderr, "Max observed UUID is %lu\n", max_nrec);
    load_count = 0;
    max_height = getMaxObservedHeight();
    fprintf(stderr, "Max Height is %d\n", max_height);

    setup_typemap();

    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.table = &table;
    rp.store = this;

    for (int hei=0; hei<=max_height; hei++)
    {
        unsigned long cur = load_count;

#if GET_ONE_BIG_BLOB
        char buff[BUFSZ];
        snprintf(buff, BUFSZ, "SELECT * FROM Atoms WHERE height = %d;", hei);
        rp.height = hei;
        rp.rs = db_conn->exec(buff);
        rp.rs->foreach_row(&Response::load_all_atoms_cb, &rp);
        rp.release();
#else
        // It appears that, when the select statement returns more than
        // about a 100K to a million atoms or so, some sort of heap
        // corruption occurs in the iodbc code, causing future mallocs
        // to fail. So limit the number of records processed in one go.
        // It also appears that asking for lots of records increases
        // the memory fragmentation (and/or there's a memory leak in iodbc??)
        // XXX Not clear is UnixODBC suffers from this same problem.
        // Whatever, seems to be a better strategy overall, anyway.
#define STEP 12003
        unsigned long rec;
        for (rec = 0; rec <= max_nrec; rec += STEP)
        {
            char buff[BUFSZ];
            snprintf(buff, BUFSZ, "SELECT * FROM Atoms WHERE "
                    "height = %d AND uuid > %lu AND uuid <= %lu;",
                     hei, rec, rec+STEP);
            rp.height = hei;
            rp.rs = db_conn->exec(buff);
            rp.rs->foreach_row(&Response::load_all_atoms_cb, &rp);
            rp.release();
        }
#endif
        fprintf(stderr, "Loaded %lu atoms at height %d\n", load_count - cur, hei);
    }
    put_conn(db_conn);
    fprintf(stderr, "Finished loading %lu atoms in total\n",
        (unsigned long) load_count);

    // synchrnonize!
    table.barrier();
}

void ODBCAtomStorage::loadType(AtomTable &table, Type atom_type)
{
    unsigned long max_nrec = getMaxObservedUUID();
    _tlbuf.reserve_upto(max_nrec);
    logger().debug("ODBCAtomStorage::loadType: Max observed UUID is %lu\n", max_nrec);
    load_count = 0;

    // For links, assume a worst-case height.
    // For nodes, its easy ... max_height is zero.
    if (classserver().isNode(atom_type))
        max_height = 0;
    else
        max_height = getMaxObservedHeight();
    logger().debug("ODBCAtomStorage::loadType: Max Height is %d\n", max_height);

    setup_typemap();
    int db_atom_type = storing_typemap[atom_type];

    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.table = &table;
    rp.store = this;

    for (int hei=0; hei<=max_height; hei++)
    {
        unsigned long cur = load_count;

#if GET_ONE_BIG_BLOB
        char buff[BUFSZ];
        snprintf(buff, BUFSZ,
            "SELECT * FROM Atoms WHERE height = %d AND type = %d;",
             hei, db_atom_type);
        rp.height = hei;
        rp.rs = db_conn->exec(buff);
        rp.rs->foreach_row(&Response::load_if_not_exists_cb, &rp);
        rp.release();
#else
        // It appears that, when the select statment returns more than
        // about a 100K to a million atoms or so, some sort of heap
        // corruption occurs in the iodbc code, causing future mallocs
        // to fail. So limit the number of records processed in one go.
        // It also appears that asking for lots of records increases
        // the memory fragmentation (and/or there's a memory leak in iodbc??)
        // XXX Not clear is UnixODBC suffers from this same problem.
#define STEP 12003
        unsigned long rec;
        for (rec = 0; rec <= max_nrec; rec += STEP)
        {
            char buff[BUFSZ];
            snprintf(buff, BUFSZ, "SELECT * FROM Atoms WHERE type = %d "
                    "AND height = %d AND uuid > %lu AND uuid <= %lu;",
                     db_atom_type, hei, rec, rec+STEP);
            rp.height = hei;
            rp.rs = db_conn->exec(buff);
            rp.rs->foreach_row(&Response::load_if_not_exists_cb, &rp);
            rp.release();
        }
#endif
        logger().debug("ODBCAtomStorage::loadType: Loaded %lu atoms of type %d at height %d\n",
            load_count - cur, db_atom_type, hei);
    }
    put_conn(db_conn);
    logger().debug("ODBCAtomStorage::loadType: Finished loading %lu atoms in total\n",
        (unsigned long) load_count);

    // Synchronize!
    table.barrier();
}

bool ODBCAtomStorage::store_cb(AtomPtr atom)
{
    storeSingleAtom(atom);
    store_count ++;
    if (store_count%1000 == 0)
    {
        fprintf(stderr, "\tStored %lu atoms.\n", (unsigned long) store_count);
    }
    return false;
}

void ODBCAtomStorage::store(const AtomTable &table)
{
    max_height = 0;
    store_count = 0;

#ifdef ALTER
    rename_tables();
    create_tables();
#endif

    get_ids();
    UUID max_uuid = _tlbuf.getMaxUUID();
    fprintf(stderr, "Max UUID is %lu\n", max_uuid);

    setup_typemap();

    ODBCConnection* db_conn = get_conn();
    Response rp;

#ifndef USE_INLINE_EDGES
    // Drop indexes, for faster loading.
    // But this only matters for the non-inline eges...
    rp.rs = db_conn->exec("DROP INDEX src_idx;");
    rp.release();
#endif

    table.foreachHandleByType(
        [&](Handle h)->void { store_cb(h); }, ATOM, true);

#ifndef USE_INLINE_EDGES
    // Create indexes
    rp.rs = db_conn->exec("CREATE INDEX src_idx ON Edges (src_uuid);");
    rp.release();
#endif /* USE_INLINE_EDGES */

    rp.rs = db_conn->exec("VACUUM ANALYZE;");
    rp.release();
    put_conn(db_conn);

    setMaxHeight(getMaxObservedHeight());
    fprintf(stderr, "\tFinished storing %lu atoms total.\n",
        (unsigned long) store_count);
}

/* ================================================================ */

void ODBCAtomStorage::rename_tables(void)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;

    rp.rs = db_conn->exec("ALTER TABLE Atoms RENAME TO Atoms_Backup;");
    rp.release();
#ifndef USE_INLINE_EDGES
    rp.rs = db_conn->exec("ALTER TABLE Edges RENAME TO Edges_Backup;");
    rp.release();
#endif /* USE_INLINE_EDGES */
    rp.rs = db_conn->exec("ALTER TABLE Global RENAME TO Global_Backup;");
    rp.release();
    rp.rs = db_conn->exec("ALTER TABLE TypeCodes RENAME TO TypeCodes_Backup;");
    rp.release();
    put_conn(db_conn);
}

void ODBCAtomStorage::create_tables(void)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;

    // See the file "atom.sql" for detailed documentation as to the
    // structure of the SQL tables.
    rp.rs = db_conn->exec("CREATE TABLE Spaces ("
                          "space     BIGINT PRIMARY KEY,"
                          "parent    BIGINT);");
    rp.release();

    rp.rs = db_conn->exec("INSERT INTO Spaces VALUES (0,0);");
    rp.release();
    rp.rs = db_conn->exec("INSERT INTO Spaces VALUES (1,1);");
    rp.release();

    rp.rs = db_conn->exec("CREATE TABLE Atoms ("
                          "uuid     BIGINT PRIMARY KEY,"
                          "space    BIGINT REFERENCES spaces(space),"
                          "type     SMALLINT,"
                          "type_tv  SMALLINT,"
                          "stv_mean FLOAT,"
                          "stv_confidence FLOAT,"
                          "stv_count DOUBLE PRECISION,"
                          "height   SMALLINT,"
                          "name     TEXT,"
                          "outgoing BIGINT[],"
                          "UNIQUE (type, name),"
                          "UNIQUE (type, outgoing));");
    rp.release();

#ifndef USE_INLINE_EDGES
    rp.rs = db_conn->exec("CREATE TABLE Edges ("
                          "src_uuid  INT,"
                          "dst_uuid  INT,"
                          "pos INT);");
    rp.release();
#endif /* USE_INLINE_EDGES */

    rp.rs = db_conn->exec("CREATE TABLE TypeCodes ("
                          "type SMALLINT UNIQUE,"
                          "typename TEXT UNIQUE);");
    rp.release();
    type_map_was_loaded = false;

    rp.rs = db_conn->exec("CREATE TABLE Global ("
                          "max_height INT);");
    rp.release();
    rp.rs = db_conn->exec("INSERT INTO Global (max_height) VALUES (0);");
    rp.release();

    put_conn(db_conn);
}

/**
 * kill_data -- destroy data in the database!! Dangerous !!
 * This routine is meant to be used only for running test cases.
 * It is extremely dangerous, as it can lead to total data loss.
 */
void ODBCAtomStorage::kill_data(void)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;

    // See the file "atom.sql" for detailed documentation as to the
    // structure of the SQL tables.
    rp.rs = db_conn->exec("DELETE from Atoms;");
    rp.release();

    // Delete the atomspaces as well!
    rp.rs = db_conn->exec("DELETE from Spaces;");
    rp.release();

    rp.rs = db_conn->exec("INSERT INTO Spaces VALUES (0,0);");
    rp.release();
    rp.rs = db_conn->exec("INSERT INTO Spaces VALUES (1,1);");
    rp.release();

    rp.rs = db_conn->exec("UPDATE Global SET max_height = 0;");
    rp.release();
    put_conn(db_conn);
}

/* ================================================================ */

void ODBCAtomStorage::setMaxHeight(int sqmax)
{
    // Max height of db contents can only get larger!
    if (max_height < sqmax) max_height = sqmax;

    char buff[BUFSZ];
    snprintf(buff, BUFSZ, "UPDATE Global SET max_height = %d;", max_height);

    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.rs = db_conn->exec(buff);
    rp.release();
    put_conn(db_conn);
}

int ODBCAtomStorage::getMaxHeight(void)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.rs = db_conn->exec("SELECT max_height FROM Global;");
    rp.rs->foreach_row(&Response::intval_cb, &rp);
    rp.release();
    put_conn(db_conn);
    return rp.intval;
}

UUID ODBCAtomStorage::getMaxObservedUUID(void)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.intval = 0;
    rp.rs = db_conn->exec("SELECT uuid FROM Atoms ORDER BY uuid DESC LIMIT 1;");
    rp.rs->foreach_row(&Response::intval_cb, &rp);
    rp.release();
    put_conn(db_conn);
    return rp.intval;
}

int ODBCAtomStorage::getMaxObservedHeight(void)
{
    ODBCConnection* db_conn = get_conn();
    Response rp;
    rp.intval = 0;
    rp.rs = db_conn->exec("SELECT height FROM Atoms ORDER BY height DESC LIMIT 1;");
    rp.rs->foreach_row(&Response::intval_cb, &rp);
    rp.release();
    put_conn(db_conn);
    return rp.intval;
}

void ODBCAtomStorage::reserve(void)
{
    UUID max_observed_id = getMaxObservedUUID();
    fprintf(stderr, "Reserving UUID up to %lu\n", max_observed_id);
    _tlbuf.reserve_upto(max_observed_id);
}

#endif /* HAVE_SQL_STORAGE */
/* ============================= END OF FILE ================= */
