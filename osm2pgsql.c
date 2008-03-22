/*
#-----------------------------------------------------------------------------
# osm2pgsql - converts planet.osm file into PostgreSQL
# compatible output suitable to be rendered by mapnik
# Use: osm2pgsql planet.osm.bz2
#-----------------------------------------------------------------------------
# Original Python implementation by Artem Pavlenko
# Re-implementation by Jon Burgess, Copyright 2006
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#-----------------------------------------------------------------------------
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>

#include <libpq-fe.h>

#include <libxml/xmlstring.h>
#include <libxml/xmlreader.h>

#include "osmtypes.h"
#include "build_geometry.h"
#include "keyvals.h"
#include "middle-pgsql.h"
#include "middle-ram.h"
#include "output-pgsql.h"
#include "sanitizer.h"
#include "reprojection.h"
#include "text-tree.h"
#include "input.h"
#include "sprompt.h"

static int count_node,    max_node;
static int count_way,     max_way;
static int count_rel,     max_rel;

struct middle_t *mid;
struct output_t *out;

/* Since {node,way} elements are not nested we can guarantee the 
   values in an end tag must match those of the corresponding 
   start tag and can therefore be cached.
*/
static double node_lon, node_lat;
static struct keyval tags, nds, members;
static int osm_id;

int verbose;

// Bounding box to filter imported data
const char *bbox = NULL;
static double minlon, minlat, maxlon, maxlat;

static void printStatus(void)
{
    fprintf(stderr, "\rProcessing: Node(%dk) Way(%dk) Relation(%dk)",
            count_node/1000, count_way/1000, count_rel/1000);
}

static int parse_bbox(void)
{
    int n;

    if (!bbox)
        return 0;

    n = sscanf(bbox, "%lf,%lf,%lf,%lf", &minlon, &minlat, &maxlon, &maxlat);
    if (n != 4) {
        fprintf(stderr, "Bounding box must be specified like: minlon,minlat,maxlon,maxlat\n");
        return 1;
    }
    if (maxlon <= minlon) {
        fprintf(stderr, "Bounding box failed due to maxlon <= minlon\n");
        return 1;
    }
    if (maxlat <= minlat) {
        fprintf(stderr, "Bounding box failed due to maxlat <= minlat\n");
        return 1;
    }
    printf("Applying Bounding box: %f,%f to %f,%f\n", minlon,minlat,maxlon,maxlat);
    return 0;
}

static int node_wanted(double lat, double lon)
{
    if (!bbox)
        return 1;

    if (lat < minlat || lat > maxlat)
        return 0;
    if (lon < minlon || lon > maxlon)
        return 0;
    return 1;
}

void StartElement(xmlTextReaderPtr reader, const xmlChar *name)
{
    xmlChar *xid, *xlat, *xlon, *xk, *xv, *xrole, *xtype;
    char *k;

    if (xmlStrEqual(name, BAD_CAST "node")) {
        xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
        xlon = xmlTextReaderGetAttribute(reader, BAD_CAST "lon");
        xlat = xmlTextReaderGetAttribute(reader, BAD_CAST "lat");
        assert(xid); assert(xlon); assert(xlat);

        osm_id  = strtol((char *)xid, NULL, 10);
        node_lon = strtod((char *)xlon, NULL);
        node_lat = strtod((char *)xlat, NULL);

        if (osm_id > max_node)
            max_node = osm_id;

        count_node++;
        if (count_node%10000 == 0)
            printStatus();

        xmlFree(xid);
        xmlFree(xlon);
        xmlFree(xlat);
    } else if (xmlStrEqual(name, BAD_CAST "tag")) {
        xk = xmlTextReaderGetAttribute(reader, BAD_CAST "k");
        assert(xk);

        /* 'created_by' and 'source' are common and not interesting to mapnik renderer */
        if (strcmp((char *)xk, "created_by") && strcmp((char *)xk, "source")) {
            char *p;
            xv = xmlTextReaderGetAttribute(reader, BAD_CAST "v");
            assert(xv);
            k  = (char *)xmlStrdup(xk);
            while ((p = strchr(k, ' ')))
                *p = '_';

            addItem(&tags, k, (char *)xv, 0);
            xmlFree(k);
            xmlFree(xv);
        }
        xmlFree(xk);
    } else if (xmlStrEqual(name, BAD_CAST "way")) {
        xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
        assert(xid);
        osm_id   = strtol((char *)xid, NULL, 10);

        if (osm_id > max_way)
            max_way = osm_id;

        count_way++;
        if (count_way%1000 == 0)
            printStatus();

        xmlFree(xid);
    } else if (xmlStrEqual(name, BAD_CAST "nd")) {
        xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "ref");
        assert(xid);

        addItem(&nds, "id", (char *)xid, 0);

        xmlFree(xid);
    } else if (xmlStrEqual(name, BAD_CAST "relation")) {
        xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
        assert(xid);
        osm_id   = strtol((char *)xid, NULL, 10);

        if (osm_id > max_rel)
            max_rel = osm_id;

        count_rel++;
        if (count_rel%1000 == 0)
            printStatus();

        xmlFree(xid);
    } else if (xmlStrEqual(name, BAD_CAST "member")) {
	xrole = xmlTextReaderGetAttribute(reader, BAD_CAST "role");
	assert(xrole);

	xtype = xmlTextReaderGetAttribute(reader, BAD_CAST "type");
	assert(xtype);

        xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "ref");
        assert(xid);

        /* Currently we are only interested in 'way' members since these form polygons with holes */
	if (xmlStrEqual(xtype, BAD_CAST "way"))
	    addItem(&members, (char *)xrole, (char *)xid, 0);

        xmlFree(xid);
        xmlFree(xrole);
        xmlFree(xtype);
    } else if (xmlStrEqual(name, BAD_CAST "osm")) {
        /* ignore */
    } else if (xmlStrEqual(name, BAD_CAST "bound")) {
        /* ignore */
    } else {
        fprintf(stderr, "%s: Unknown element name: %s\n", __FUNCTION__, name);
    }
}

void EndElement(const xmlChar *name)
{
    if (xmlStrEqual(name, BAD_CAST "node")) {
        if (node_wanted(node_lat, node_lon)) {
            reproject(&node_lat, &node_lon);
            mid->nodes_set(osm_id, node_lat, node_lon, &tags);
        }
        resetList(&tags);
    } else if (xmlStrEqual(name, BAD_CAST "way")) {
        mid->ways_set(osm_id, &nds, &tags);
        resetList(&tags);
        resetList(&nds);
    } else if (xmlStrEqual(name, BAD_CAST "relation")) {
        mid->relations_set(osm_id, &members, &tags);
        resetList(&tags);
        resetList(&members);
    } else if (xmlStrEqual(name, BAD_CAST "tag")) {
        /* ignore */
    } else if (xmlStrEqual(name, BAD_CAST "nd")) {
        /* ignore */
    } else if (xmlStrEqual(name, BAD_CAST "member")) {
	/* ignore */
    } else if (xmlStrEqual(name, BAD_CAST "osm")) {
        printStatus();
    } else if (xmlStrEqual(name, BAD_CAST "bound")) {
        /* ignore */
    } else {
        fprintf(stderr, "%s: Unknown element name: %s\n", __FUNCTION__, name);
    }
}

static void processNode(xmlTextReaderPtr reader) {
    xmlChar *name;
    name = xmlTextReaderName(reader);
    if (name == NULL)
        name = xmlStrdup(BAD_CAST "--");
	
    switch(xmlTextReaderNodeType(reader)) {
        case XML_READER_TYPE_ELEMENT:
            StartElement(reader, name);
            if (xmlTextReaderIsEmptyElement(reader))
                EndElement(name); /* No end_element for self closing tags! */
            break;
        case XML_READER_TYPE_END_ELEMENT:
            EndElement(name);
            break;
        case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
            /* Ignore */
            break;
        default:
            fprintf(stderr, "Unknown node type %d\n", xmlTextReaderNodeType(reader));
            break;
    }

    xmlFree(name);
}

static int streamFile(char *filename, int sanitize) {
    xmlTextReaderPtr reader;
    int ret = 0;

    if (sanitize)
        reader = sanitizerOpen(filename);
    else
        reader = inputUTF8(filename);

    if (reader != NULL) {
        ret = xmlTextReaderRead(reader);
        while (ret == 1) {
            processNode(reader);
            ret = xmlTextReaderRead(reader);
        }

        if (ret != 0) {
            fprintf(stderr, "%s : failed to parse\n", filename);
            return ret;
        }

        xmlFreeTextReader(reader);
    } else {
        fprintf(stderr, "Unable to open %s\n", filename);
        return 1;
    }
    return 0;
}

void exit_nicely(void)
{
    fprintf(stderr, "Error occurred, cleaning up\n");
    out->cleanup();
    mid->cleanup();
    exit(1);
}
 
static void usage(const char *arg0)
{
    int i;
    const char *name = basename(arg0);

    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "\t%s [options] planet.osm\n", name);
    fprintf(stderr, "\t%s [options] planet.osm.{gz,bz2}\n", name);
    fprintf(stderr, "\t%s [options] file1.osm file2.osm file3.osm\n", name);
    fprintf(stderr, "\nThis will import the data from the OSM file(s) into a PostgreSQL database\n");
    fprintf(stderr, "suitable for use by the Mapnik renderer\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "   -a|--append\t\tAdd the OSM file into the database without removing\n");
    fprintf(stderr, "              \t\texisting data.\n");
    fprintf(stderr, "   -b|--bbox\t\tApply a bounding box filter on the imported data\n");
    fprintf(stderr, "              \t\tMust be specified as: minlon,minlat,maxlon,maxlat\n");
    fprintf(stderr, "              \t\te.g. --bbox -0.5,51.25,0.5,51.75\n");
    fprintf(stderr, "   -c|--create\t\tRemove existing data from the database. This is the \n");
    fprintf(stderr, "              \t\tdefault if --append is not specified.\n");
    fprintf(stderr, "   -d|--database\tThe name of the PostgreSQL database to connect\n");
    fprintf(stderr, "                \tto (default: gis).\n");
    fprintf(stderr, "   -l|--latlong\t\tStore data in degrees of latitude & longitude.\n");
    fprintf(stderr, "   -m|--merc\t\tStore data in proper spherical mercator, not OSM merc\n");
    fprintf(stderr, "   -E|--proj num\tUse projection EPSG:num\n");
    fprintf(stderr, "   -u|--utf8-sanitize\tRepair bad UTF8 input data (present in planet\n");
    fprintf(stderr, "                \tdumps prior to August 2007). Adds about 10%% overhead.\n");
    fprintf(stderr, "   -p|--prefix\t\tPrefix for table names (default planet_osm)\n");
#ifdef BROKEN_SLIM
    fprintf(stderr, "   -s|--slim\t\tStore temporary data in the database. This greatly\n");
    fprintf(stderr, "            \t\treduces the RAM usage but is much slower.\n");
#endif
    fprintf(stderr, "   -U|--username\tPostgresql user name.\n");
    fprintf(stderr, "   -W|--password\tForce password prompt.\n");
    fprintf(stderr, "   -H|--host\t\tDatabase server hostname or socket location.\n");
    fprintf(stderr, "   -P|--port\t\tDatabase server port.\n");
    fprintf(stderr, "   -h|--help\t\tHelp information.\n");
    fprintf(stderr, "   -v|--verbose\t\tVerbose output.\n");
    fprintf(stderr, "\n");
    if(!verbose)
    {
        fprintf(stderr, "Add -v to display supported projections.\n");
        fprintf(stderr, "Use -E to access any espg projections (usually in /usr/share/proj/epsg)\n" );
    }
    else
    {
        fprintf(stderr, "Supported projections:\n" );
        for(i=0; i<PROJ_COUNT; i++ )
        {
            fprintf( stderr, "%-20s(%2s) SRS:%6d %s\n", 
                    Projection_Info[i].descr, Projection_Info[i].option, Projection_Info[i].srs, Projection_Info[i].proj4text);
        }
    }
}

const char *build_conninfo(const char *db, const char *username, const char *password, const char *host, const char *port)
{
    static char conninfo[1024];

    conninfo[0]='\0';
    strcat(conninfo, "dbname='");
    strcat(conninfo, db);
    strcat(conninfo, "'");

    if (username) {
        strcat(conninfo, " user='");
        strcat(conninfo, username);
        strcat(conninfo, "'");
    }
    if (password) {
        strcat(conninfo, " password='");
        strcat(conninfo, password);
        strcat(conninfo, "'");
    }
    if (host) {
        strcat(conninfo, " host='");
        strcat(conninfo, host);
        strcat(conninfo, "'");
    }
    if (port) {
        strcat(conninfo, " port='");
        strcat(conninfo, port);
        strcat(conninfo, "'");
    }

    return conninfo;
}

int main(int argc, char *argv[])
{
    int append=0;
    int create=0;
    int slim=0;
    int sanitize=0;
    int pass_prompt=0;
    int projection = PROJ_MERC;
    const char *db = "gis";
    const char *username=NULL;
    const char *host=NULL;
    const char *password=NULL;
    const char *port = "5432";
    const char *conninfo = NULL;
    const char *prefix = "planet_osm";
    PGconn *sql_conn;

    fprintf(stderr, "osm2pgsql SVN version %s $Rev$ \n\n", VERSION);

    while (1) {
        int c, option_index = 0;
        static struct option long_options[] = {
            {"append",   0, 0, 'a'},
            {"bbox",     1, 0, 'b'},
            {"create",   0, 0, 'c'},
            {"database", 1, 0, 'd'},
            {"latlong",  0, 0, 'l'},
            {"verbose",  0, 0, 'v'},
#ifdef BROKEN_SLIM
            {"slim",     0, 0, 's'},
#endif
            {"prefix",   1, 0, 'p'},
            {"proj",     1, 0, 'E'},
            {"merc",     0, 0, 'm'},
            {"utf8-sanitize", 0, 0, 'u'},
            {"username", 1, 0, 'U'},
            {"password", 0, 0, 'W'},
            {"host",     1, 0, 'H'},
            {"port",     1, 0, 'P'},
            {"help",     0, 0, 'h'},
            {0, 0, 0, 0}
        };

        c = getopt_long (argc, argv, "ab:cd:hlmp:suvU:WH:P:E:", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'a': append=1;   break;
            case 'b': bbox=optarg; break;
            case 'c': create=1;   break;
            case 'v': verbose=1;  break;
#ifdef BROKEN_SLIM
            case 's': slim=1;     break;
#endif
            case 'u': sanitize=1; break;
            case 'l': projection=PROJ_LATLONG;  break;
            case 'm': projection=PROJ_SPHERE_MERC; break;
            case 'E': projection=-atoi(optarg); break;
            case 'p': prefix=optarg; break;
            case 'd': db=optarg;  break;
            case 'U': username=optarg; break;
            case 'W': pass_prompt=1; break;
            case 'H': host=optarg; break;
            case 'P': port=optarg; break;

            case 'h':
            case '?':
            default:
                usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (argc == optind) {  // No non-switch arguments
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (append && create) {
        fprintf(stderr, "Error: --append and --create options can not be used at the same time!\n");
        exit(EXIT_FAILURE);
    }

    if (username || pass_prompt)
        password = simple_prompt("Password:", 100, 0);

    conninfo = build_conninfo(db, username, password, host, port);
    sql_conn = PQconnectdb(conninfo);
    if (PQstatus(sql_conn) != CONNECTION_OK) {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(sql_conn));
        exit(EXIT_FAILURE);
    }
    PQfinish(sql_conn);

    text_init();
    initList(&tags);
    initList(&nds);
    initList(&members);

    count_node = max_node = 0;
    count_way = max_way = 0;
    count_rel = max_rel = 0;

    LIBXML_TEST_VERSION

    project_init(projection);
    fprintf(stderr, "Using projection SRS %d (%s)\n", 
        project_getprojinfo()->srs, project_getprojinfo()->descr );

    if (parse_bbox())
        return 1;

    mid = slim ? &mid_pgsql : &mid_ram;
    out = &out_pgsql;

    out->start(conninfo, prefix, append);

    while (optind < argc) {
        fprintf(stderr, "\nReading in file: %s\n", argv[optind]);
        mid->start(conninfo, projection==PROJ_LATLONG);
        if (streamFile(argv[optind], sanitize) != 0)
            exit_nicely();
        mid->end();
        mid->analyze();

        //mid->iterate_nodes(out->node);
        mid->iterate_relations(out->relation);
        mid->iterate_ways(out->way);
        mid->stop();
        optind++;
    }

    xmlCleanupParser();
    xmlMemoryDump();

    if (count_node || count_way || count_rel) {
        fprintf(stderr, "\n");
        fprintf(stderr, "Node stats: total(%d), max(%d)\n", count_node, max_node);
        fprintf(stderr, "Way stats: total(%d), max(%d)\n", count_way, max_way);
        fprintf(stderr, "Relation stats: total(%d), max(%d)\n", count_rel, max_rel);
    }
    //fprintf(stderr, "\n\nEnding data import\n");
    //out->process(mid);
    out->stop(append);

    project_exit();
    text_exit();
    fprintf(stderr, "\n");

    return 0;
}
