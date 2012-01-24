/*

 gg_gml.c -- GML parser/lexer 
  
 version 3.0, 2011 July 20

 Author: Sandro Furieri a.furieri@lqt.it

 ------------------------------------------------------------------------------
 
 Version: MPL 1.1/GPL 2.0/LGPL 2.1
 
 The contents of this file are subject to the Mozilla Public License Version
 1.1 (the "License"); you may not use this file except in compliance with
 the License. You may obtain a copy of the License at
 http://www.mozilla.org/MPL/
 
Software distributed under the License is distributed on an "AS IS" basis,
WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
for the specific language governing rights and limitations under the
License.

The Original Code is the SpatiaLite library

The Initial Developer of the Original Code is Alessandro Furieri
 
Portions created by the Initial Developer are Copyright (C) 2011
the Initial Developer. All Rights Reserved.

Alternatively, the contents of this file may be used under the terms of
either the GNU General Public License Version 2 or later (the "GPL"), or
the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
in which case the provisions of the GPL or the LGPL are applicable instead
of those above. If you wish to allow use of your version of this file only
under the terms of either the GPL or the LGPL, and not to allow others to
use your version of this file under the terms of the MPL, indicate your
decision by deleting the provisions above and replace them with the notice
and other provisions required by the GPL or the LGPL. If you do not delete
the provisions above, a recipient may use your version of this file under
the terms of any one of the MPL, the GPL or the LGPL.
 
*/

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <assert.h>

#ifdef SPL_AMALGAMATION		/* spatialite-amalgamation */
#include <spatialite/sqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <spatialite/gaiageo.h>

#if defined(_WIN32) || defined(WIN32)
#include <io.h>
#ifndef isatty
#define isatty	_isatty
#endif
#ifndef fileno
#define fileno	_fileno
#endif
#endif

int gml_parse_error;

#define GML_PARSER_OPEN_NODE		1
#define GML_PARSER_SELF_CLOSED_NODE	2
#define GML_PARSER_CLOSED_NODE		3

#define GAIA_GML_UNKNOWN		0
#define GAIA_GML_POINT			1
#define GAIA_GML_LINESTRING		2
#define GAIA_GML_CURVE			3
#define GAIA_GML_POLYGON		4
#define GAIA_GML_MULTIPOINT		5
#define GAIA_GML_MULTILINESTRING	6
#define GAIA_GML_MULTICURVE		7
#define GAIA_GML_MULTIPOLYGON		8
#define GAIA_GML_MULTISURFACE		9
#define GAIA_GML_MULTIGEOMETRY		10

/*
** This is a linked-list struct to store all the values for each token.
*/
typedef struct gmlFlexTokenStruct
{
    char *value;
    struct gmlFlexTokenStruct *Next;
} gmlFlexToken;

typedef struct gml_coord
{
    char *Value;
    struct gml_coord *Next;
} gmlCoord;
typedef gmlCoord *gmlCoordPtr;

typedef struct gml_attr
{
    char *Key;
    char *Value;
    struct gml_attr *Next;
} gmlAttr;
typedef gmlAttr *gmlAttrPtr;

typedef struct gml_node
{
    char *Tag;
    int Type;
    int Error;
    struct gml_attr *Attributes;
    struct gml_coord *Coordinates;
    struct gml_node *Next;
} gmlNode;
typedef gmlNode *gmlNodePtr;

typedef struct gml_dynamic_ring
{
    gaiaDynamicLinePtr ring;
    int interior;
    int has_z;
    struct gml_dynamic_ring *next;
} gmlDynamicRing;
typedef gmlDynamicRing *gmlDynamicRingPtr;

typedef struct gml_dynamic_polygon
{
    struct gml_dynamic_ring *first;
    struct gml_dynamic_ring *last;
} gmlDynamicPolygon;
typedef gmlDynamicPolygon *gmlDynamicPolygonPtr;

static void
gml_proj_params (sqlite3 * sqlite, int srid, char *proj_params)
{
/* retrives the PROJ params from SPATIAL_SYS_REF table, if possible */
    char sql[256];
    char **results;
    int rows;
    int columns;
    int i;
    int ret;
    char *errMsg = NULL;
    *proj_params = '\0';
    sprintf (sql,
	     "SELECT proj4text FROM spatial_ref_sys WHERE srid = %d", srid);
    ret = sqlite3_get_table (sqlite, sql, &results, &rows, &columns, &errMsg);
    if (ret != SQLITE_OK)
      {
	  fprintf (stderr, "unknown SRID: %d\t<%s>\n", srid, errMsg);
	  sqlite3_free (errMsg);
	  return;
      }
    for (i = 1; i <= rows; i++)
	strcpy (proj_params, results[(i * columns)]);
    if (*proj_params == '\0')
	fprintf (stderr, "unknown SRID: %d\n", srid);
    sqlite3_free_table (results);
}

static gmlDynamicPolygonPtr
gml_alloc_dyn_polygon (void)
{
/* creating a dynamic polygon (ring collection) */
    gmlDynamicPolygonPtr p = malloc (sizeof (gmlDynamicPolygon));
    p->first = NULL;
    p->last = NULL;
    return p;
}

static void
gml_free_dyn_polygon (gmlDynamicPolygonPtr dyn)
{
/* deleting a dynamic polygon (ring collection) */
    gmlDynamicRingPtr r;
    gmlDynamicRingPtr rn;
    if (!dyn)
	return;
    r = dyn->first;
    while (r)
      {
	  rn = r->next;
	  if (r->ring)
	      gaiaFreeDynamicLine (r->ring);
	  free (r);
	  r = rn;
      }
    free (dyn);
}

static void
gml_add_polygon_ring (gmlDynamicPolygonPtr dyn_pg, gaiaDynamicLinePtr dyn,
		      int interior, int has_z)
{
/* inserting a further ring into the collection (dynamic polygon) */
    gmlDynamicRingPtr p = malloc (sizeof (gmlDynamicRing));
    p->ring = dyn;
    p->interior = interior;
    p->has_z = has_z;
    p->next = NULL;
    if (dyn_pg->first == NULL)
	dyn_pg->first = p;
    if (dyn_pg->last != NULL)
	dyn_pg->last->next = p;
    dyn_pg->last = p;
}

static void
gml_freeString (char **ptr)
{
/* releasing a string from the lexer */
    if (*ptr != NULL)
	free (*ptr);
    *ptr = NULL;
}

static void
gml_saveString (char **ptr, const char *str)
{
/* saving a string from the lexer */
    int len = strlen (str);
    gml_freeString (ptr);
    *ptr = malloc (len + 1);
    strcpy (*ptr, str);
}

static gmlCoordPtr
gml_coord (void *value)
{
/* creating a coord Item */
    int len;
    gmlFlexToken *tok = (gmlFlexToken *) value;
    gmlCoordPtr c = malloc (sizeof (gmlCoord));
    len = strlen (tok->value);
    c->Value = malloc (len + 1);
    strcpy (c->Value, tok->value);
    c->Next = NULL;
    return c;
}

static void
gml_freeCoordinate (gmlCoordPtr c)
{
/* deleting a GML coordinate */
    if (c == NULL)
	return;
    if (c->Value)
	free (c->Value);
    free (c);
}

static gmlAttrPtr
gml_attribute (void *key, void *value)
{
/* creating an attribute */
    int len;
    gmlFlexToken *k_tok = (gmlFlexToken *) key;
    gmlFlexToken *v_tok = (gmlFlexToken *) value;
    gmlAttrPtr a = malloc (sizeof (gmlAttr));
    len = strlen (k_tok->value);
    a->Key = malloc (len + 1);
    strcpy (a->Key, k_tok->value);
    len = strlen (v_tok->value);
/* we need to de-quote the string, removing first and last ".." */
    if (*(v_tok->value + 0) == '"' && *(v_tok->value + len - 1) == '"')
      {
	  a->Value = malloc (len - 1);
	  memcpy (a->Value, v_tok->value + 1, len - 1);
	  *(a->Value + len - 1) = '\0';
      }
    else
      {
	  a->Value = malloc (len + 1);
	  strcpy (a->Value, v_tok->value);
      }
    a->Next = NULL;
    return a;
}

static void
gml_freeAttribute (gmlAttrPtr a)
{
/* deleting a GML attribute */
    if (a == NULL)
	return;
    if (a->Key)
	free (a->Key);
    if (a->Value)
	free (a->Value);
    free (a);
}

static void
gml_freeNode (gmlNodePtr n)
{
/* deleting a GML node */
    gmlAttrPtr a;
    gmlAttrPtr an;
    gmlCoordPtr c;
    gmlCoordPtr cn;
    if (n == NULL)
	return;
    a = n->Attributes;
    while (a)
      {
	  an = a->Next;
	  gml_freeAttribute (a);
	  a = an;
      }
    c = n->Coordinates;
    while (c)
      {
	  cn = c->Next;
	  gml_freeCoordinate (c);
	  c = cn;
      }
    if (n->Tag)
	free (n->Tag);
    free (n);
}

static void
gml_freeTree (gmlNodePtr t)
{
/* deleting a GML tree */
    gmlNodePtr n;
    gmlNodePtr nn;
    n = t;
    while (n)
      {
	  nn = n->Next;
	  gml_freeNode (n);
	  n = nn;
      }
}

static gmlNodePtr
gml_createNode (void *tag, void *attributes, void *coords)
{
/* creating a node */
    int len;
    gmlFlexToken *tok = (gmlFlexToken *) tag;
    gmlNodePtr n = malloc (sizeof (gmlNode));
    len = strlen (tok->value);
    n->Tag = malloc (len + 1);
    strcpy (n->Tag, tok->value);
    n->Type = GML_PARSER_OPEN_NODE;
    n->Error = 0;
    n->Attributes = attributes;
    n->Coordinates = coords;
    n->Next = NULL;
    return n;
}

static gmlNodePtr
gml_createSelfClosedNode (void *tag, void *attributes)
{
/* creating a self-closed node */
    int len;
    gmlFlexToken *tok = (gmlFlexToken *) tag;
    gmlNodePtr n = malloc (sizeof (gmlNode));
    len = strlen (tok->value);
    n->Tag = malloc (len + 1);
    strcpy (n->Tag, tok->value);
    n->Type = GML_PARSER_SELF_CLOSED_NODE;
    n->Error = 0;
    n->Attributes = attributes;
    n->Coordinates = NULL;
    n->Next = NULL;
    return n;
}

static gmlNodePtr
gml_closingNode (void *tag)
{
/* creating a closing node */
    int len;
    gmlFlexToken *tok = (gmlFlexToken *) tag;
    gmlNodePtr n = malloc (sizeof (gmlNode));
    len = strlen (tok->value);
    n->Tag = malloc (len + 1);
    strcpy (n->Tag, tok->value);
    n->Type = GML_PARSER_CLOSED_NODE;
    n->Error = 0;
    n->Attributes = NULL;
    n->Coordinates = NULL;
    n->Next = NULL;
    return n;
}

static int
gml_cleanup (gmlFlexToken * token)
{
    gmlFlexToken *ptok;
    gmlFlexToken *ptok_n;
    if (token == NULL)
	return 0;
    ptok = token;
    while (ptok)
      {
	  ptok_n = ptok->Next;
	  if (ptok->value != NULL)
	      free (ptok->value);
	  free (ptok);
	  ptok = ptok_n;
      }
    return 0;
}

static void
gml_xferString (char **p, const char *str)
{
/* saving some token */
    int len;
    if (str == NULL)
      {
	  *p = NULL;
	  return;
      }
    len = strlen (str);
    *p = malloc (len + 1);
    strcpy (*p, str);
}

static int
guessGmlSrid (gmlNodePtr node)
{
/* attempting to guess the SRID */
    int len;
    gmlAttrPtr attr = node->Attributes;
    while (attr)
      {
	  if (strcmp (attr->Key, "srsName") == 0)
	    {
		len = strlen (attr->Value);
		if (len > 5)
		  {
		      if (strncmp (attr->Value, "EPSG:", 5) == 0)
			  return atoi (attr->Value + 5);
		  }
		if (len > 21)
		  {
		      if (strncmp (attr->Value, "urn:ogc:def:crs:EPSG:", 21) ==
			  0)
			{
			    int i = strlen (attr->Value) - 1;
			    for (; i >= 0; i--)
			      {
				  if (*(attr->Value + i) == ':')
				      return atoi (attr->Value + i + 1);
			      }
			}
		  }
	    }
	  attr = attr->Next;
      }
    return -1;
}

static int
gml_get_srsDimension (gmlNodePtr node)
{
/* attempting to establis if there is a Z coordinate */
    gmlAttrPtr attr = node->Attributes;
    while (attr)
      {
	  if (strcmp (attr->Key, "srsDimension") == 0)
	    {
		if (atoi (attr->Value) == 3)
		    return 1;
		else
		    return 0;
	    }
	  attr = attr->Next;
      }
    return 0;
}

static int
guessGmlGeometryType (gmlNodePtr node)
{
/* attempting to guess the Geometry Type for a GML node */
    int type = GAIA_GML_UNKNOWN;
    if (strcmp (node->Tag, "gml:Point") == 0
	|| strcmp (node->Tag, "Point") == 0)
	type = GAIA_GML_POINT;
    if (strcmp (node->Tag, "gml:LineString") == 0
	|| strcmp (node->Tag, "LineString") == 0)
	type = GAIA_GML_LINESTRING;
    if (strcmp (node->Tag, "gml:Curve") == 0
	|| strcmp (node->Tag, "Curve") == 0)
	type = GAIA_GML_CURVE;
    if (strcmp (node->Tag, "gml:Polygon") == 0
	|| strcmp (node->Tag, "Polygon") == 0)
	type = GAIA_GML_POLYGON;
    if (strcmp (node->Tag, "gml:MultiPoint") == 0
	|| strcmp (node->Tag, "MultiPoint") == 0)
	type = GAIA_GML_MULTIPOINT;
    if (strcmp (node->Tag, "gml:MultiLineString") == 0
	|| strcmp (node->Tag, "MultiLineString") == 0)
	type = GAIA_GML_MULTILINESTRING;
    if (strcmp (node->Tag, "gml:MultiCurve") == 0
	|| strcmp (node->Tag, "MultiCurve") == 0)
	type = GAIA_GML_MULTICURVE;
    if (strcmp (node->Tag, "gml:MultiPolygon") == 0
	|| strcmp (node->Tag, "MultiPolygon") == 0)
	type = GAIA_GML_MULTIPOLYGON;
    if (strcmp (node->Tag, "gml:MultiSurface") == 0
	|| strcmp (node->Tag, "MultiSurface") == 0)
	type = GAIA_GML_MULTISURFACE;
    if (strcmp (node->Tag, "gml:MultiGeometry") == 0
	|| strcmp (node->Tag, "MultiGeometry") == 0)
	type = GAIA_GML_MULTIGEOMETRY;
    return type;
}

static int
gml_check_coord (const char *value)
{
/* checking a GML coordinate */
    int decimal = 0;
    const char *p = value;
    if (*p == '+' || *p == '-')
	p++;
    while (*p != '\0')
      {
	  if (*p == '.')
	    {
		if (!decimal)
		    decimal = 1;
		else
		    return 0;
	    }
	  else if (*p >= '0' && *p <= '9')
	      ;
	  else
	      return 0;
	  p++;
      }
    return 1;
}

static int
gml_extract_coords (const char *value, double *x, double *y, double *z,
		    int *count)
{
/* extracting GML v2.x coords from a comma-separated string */
    const char *in = value;
    char buf[1024];
    char *out = buf;
    *out = '\0';

    while (*in != '\0')
      {
	  if (*in == ',')
	    {
		*out = '\0';
		if (*buf != '\0')
		  {
		      if (!gml_check_coord (buf))
			  return 0;
		      switch (*count)
			{
			case 0:
			    *x = atof (buf);
			    *count += 1;
			    break;
			case 1:
			    *y = atof (buf);
			    *count += 1;
			    break;
			case 2:
			    *z = atof (buf);
			    *count += 1;
			    break;
			default:
			    *count += 1;
			    break;
			};
		  }
		in++;
		out = buf;
		*out = '\0';
		continue;
	    }
	  *out++ = *in++;
      }
    *out = '\0';
/* parsing the last item */
    if (*buf != '\0')
      {
	  if (!gml_check_coord (buf))
	      return 0;
	  switch (*count)
	    {
	    case 0:
		*x = atof (buf);
		*count += 1;
		break;
	    case 1:
		*y = atof (buf);
		*count += 1;
		break;
	    case 2:
		*z = atof (buf);
		*count += 1;
		break;
	    default:
		*count += 1;
		break;
	    };
      }
    return 1;
}

static int
gml_parse_point_v2 (gmlCoordPtr coord, double *x, double *y, double *z,
		    int *has_z)
{
/* parsing GML v2.x <gml:coordinates> [Point] */
    int count = 0;
    gmlCoordPtr c = coord;
    while (c)
      {
	  if (!gml_extract_coords (c->Value, x, y, z, &count))
	      return 0;
	  c = c->Next;
      }
    if (count == 2)
      {
	  *has_z = 0;
	  return 1;
      }
    if (count == 3)
      {
	  *has_z = 1;
	  return 1;
      }
    return 0;
}

static int
gml_parse_point_v3 (gmlCoordPtr coord, double *x, double *y, double *z,
		    int *has_z)
{
/* parsing GML v2.x <gml:pos> [Point] */
    int count = 0;
    gmlCoordPtr c = coord;
    while (c)
      {
	  if (!gml_check_coord (c->Value))
	      return 0;
	  switch (count)
	    {
	    case 0:
		*x = atof (c->Value);
		count++;
		break;
	    case 1:
		*y = atof (c->Value);
		count++;
		break;
	    case 2:
		*z = atof (c->Value);
		count++;
		break;
	    default:
		count++;
		break;
	    };
	  c = c->Next;
      }
    if (count == 2)
      {
	  *has_z = 0;
	  return 1;
      }
    if (count == 3)
      {
	  *has_z = 1;
	  return 1;
      }
    return 0;
}

static int
gml_parse_point (gaiaGeomCollPtr geom, gmlNodePtr node, int srid,
		 gmlNodePtr * next)
{
/* parsing a <gml:Point> */
    double x;
    double y;
    double z;
    int has_z;
    gaiaGeomCollPtr pt;
    gaiaGeomCollPtr last;

    if (strcmp (node->Tag, "gml:coordinates") == 0
	|| strcmp (node->Tag, "coordinates") == 0)
      {
	  /* parsing a GML v.2.x <gml:Point> */
	  if (!gml_parse_point_v2 (node->Coordinates, &x, &y, &z, &has_z))
	      return 0;
	  node = node->Next;
	  if (node == NULL)
	      return 0;
	  if (strcmp (node->Tag, "gml:coordinates") == 0
	      || strcmp (node->Tag, "coordinates") == 0)
	      ;
	  else
	      return 0;
	  node = node->Next;
	  if (node == NULL)
	      return 0;
	  if (strcmp (node->Tag, "gml:Point") == 0
	      || strcmp (node->Tag, "Point") == 0)
	      ;
	  else
	      return 0;
	  *next = node->Next;
	  goto ok;
      }
    if (strcmp (node->Tag, "gml:pos") == 0 || strcmp (node->Tag, "pos") == 0)
      {
	  /* parsing a GML v.3.x <gml:Point> */
	  if (!gml_parse_point_v3 (node->Coordinates, &x, &y, &z, &has_z))
	      return 0;
	  node = node->Next;
	  if (node == NULL)
	      return 0;
	  if (strcmp (node->Tag, "gml:pos") == 0
	      || strcmp (node->Tag, "pos") == 0)
	      ;
	  else
	      return 0;
	  node = node->Next;
	  if (node == NULL)
	      return 0;
	  if (strcmp (node->Tag, "gml:Point") == 0
	      || strcmp (node->Tag, "Point") == 0)
	      ;
	  else
	      return 0;
	  *next = node->Next;
	  goto ok;
      }
    return 0;

  ok:
/* ok, GML nodes match as expected */
    if (has_z)
      {
	  pt = gaiaAllocGeomCollXYZ ();
	  pt->Srid = srid;
	  gaiaAddPointToGeomCollXYZ (pt, x, y, z);
      }
    else
      {
	  pt = gaiaAllocGeomColl ();
	  pt->Srid = srid;
	  gaiaAddPointToGeomColl (pt, x, y);
      }
    last = geom;
    while (1)
      {
	  /* searching the last Geometry within chain */
	  if (last->Next == NULL)
	      break;
	  last = last->Next;
      }
    last->Next = pt;
    return 1;
}

static int
gml_extract_multi_coord (const char *value, double *x, double *y, double *z,
			 int *count, int *follow)
{
/* extracting GML v2.x coords from a comma-separated string */
    const char *in = value;
    char buf[1024];
    char *out = buf;
    int last;
    *out = '\0';
    while (*in != '\0')
      {
	  last = *in;
	  if (*in == ',')
	    {
		*out = '\0';
		if (*buf != '\0')
		  {
		      if (!gml_check_coord (buf))
			  return 0;
		      switch (*count)
			{
			case 0:
			    *x = atof (buf);
			    *count += 1;
			    break;
			case 1:
			    *y = atof (buf);
			    *count += 1;
			    break;
			case 2:
			    *z = atof (buf);
			    *count += 1;
			    break;
			default:
			    *count += 1;
			    break;
			};
		  }
		in++;
		out = buf;
		*out = '\0';
		continue;
	    }
	  *out++ = *in++;
      }
    *out = '\0';
/* parsing the last item */
    if (*buf != '\0')
      {
	  if (!gml_check_coord (buf))
	      return 0;
	  switch (*count)
	    {
	    case 0:
		*x = atof (buf);
		*count += 1;
		break;
	    case 1:
		*y = atof (buf);
		*count += 1;
		break;
	    case 2:
		*z = atof (buf);
		*count += 1;
		break;
	    default:
		*count += 1;
		break;
	    };
      }
    if (last == ',')
	*follow = 1;
    else
	*follow = 0;
    return 1;
}

static int
gml_extract_multi_coords (gmlCoordPtr coord, double *x, double *y, double *z,
			  int *count, gmlCoordPtr * next)
{
/* extracting GML v2.x coords from a comma-separated string */
    int follow;
    gmlCoordPtr c = coord;
    while (c)
      {
	  if (!gml_extract_multi_coord (c->Value, x, y, z, count, &follow))
	      return 0;
	  if (!follow && c->Next != NULL)
	    {
		if (*(c->Next->Value) == ',')
		    follow = 1;
	    }
	  if (follow)
	      c = c->Next;
	  else
	    {
		*next = c->Next;
		break;
	    }
      }
    return 1;
}

static void
gml_add_point_to_line (gaiaDynamicLinePtr dyn, double x, double y)
{
/* appending a point */
    gaiaAppendPointToDynamicLine (dyn, x, y);
}

static void
gml_add_point_to_lineZ (gaiaDynamicLinePtr dyn, double x, double y, double z)
{
/* appending a point */
    gaiaAppendPointZToDynamicLine (dyn, x, y, z);
}

static int
gml_parse_coordinates (gmlCoordPtr coord, gaiaDynamicLinePtr dyn, int *has_z)
{
/* parsing GML v2.x <gml:coordinates> [Linestring or Ring] */
    int count = 0;
    double x;
    double y;
    double z;
    gmlCoordPtr next;
    gmlCoordPtr c = coord;
    while (c)
      {
	  if (!gml_extract_multi_coords (c, &x, &y, &z, &count, &next))
	      return 0;
	  if (count == 2)
	    {
		*has_z = 0;
		gml_add_point_to_line (dyn, x, y);
		count = 0;
	    }
	  else if (count == 3)
	    {
		gml_add_point_to_lineZ (dyn, x, y, z);
		count = 0;
	    }
	  else
	      return 0;
	  c = next;
      }
    return 1;
}

static int
gml_parse_posList (gmlCoordPtr coord, gaiaDynamicLinePtr dyn, int has_z)
{
/* parsing GML v3.x <gml:posList> [Linestring or Ring] */
    int count = 0;
    double x;
    double y;
    double z;
    gmlCoordPtr c = coord;
    while (c)
      {
	  if (!gml_check_coord (c->Value))
	      return 0;
	  if (!has_z)
	    {
		switch (count)
		  {
		  case 0:
		      x = atof (c->Value);
		      count++;
		      break;
		  case 1:
		      y = atof (c->Value);
		      gml_add_point_to_line (dyn, x, y);
		      count = 0;
		      break;
		  };
	    }
	  else
	    {
		switch (count)
		  {
		  case 0:
		      x = atof (c->Value);
		      count++;
		      break;
		  case 1:
		      y = atof (c->Value);
		      count++;
		      break;
		  case 2:
		      z = atof (c->Value);
		      gml_add_point_to_lineZ (dyn, x, y, z);
		      count = 0;
		      break;
		  };
	    }
	  c = c->Next;
      }
    if (count != 0)
	return 0;
    return 1;
}

static int
gml_count_dyn_points (gaiaDynamicLinePtr dyn)
{
/* count how many vertices are into sone linestring/ring */
    int iv = 0;
    gaiaPointPtr pt = dyn->First;
    while (pt)
      {
	  iv++;
	  pt = pt->Next;
      }
    return iv;
}

static int
gml_parse_linestring (gaiaGeomCollPtr geom, gmlNodePtr node, int srid,
		      gmlNodePtr * next)
{
/* parsing a <gml:LineString> */
    gaiaGeomCollPtr ln;
    gaiaGeomCollPtr last;
    gaiaLinestringPtr new_ln;
    gaiaPointPtr pt;
    gaiaDynamicLinePtr dyn = gaiaAllocDynamicLine ();
    int iv;
    int has_z = 1;
    int points = 0;

    if (strcmp (node->Tag, "gml:coordinates") == 0
	|| strcmp (node->Tag, "coordinates") == 0)
      {
	  /* parsing a GML v.2.x <gml:LineString> */
	  if (!gml_parse_coordinates (node->Coordinates, dyn, &has_z))
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:coordinates") == 0
	      || strcmp (node->Tag, "coordinates") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LineString") == 0
	      || strcmp (node->Tag, "LineString") == 0)
	      ;
	  else
	      goto error;
	  *next = node->Next;
	  goto ok;
      }
    if (strcmp (node->Tag, "gml:posList") == 0
	|| strcmp (node->Tag, "posList") == 0)
      {
	  /* parsing a GML v.3.x <gml:LineString> */
	  has_z = gml_get_srsDimension (node);
	  if (!gml_parse_posList (node->Coordinates, dyn, has_z))
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LineString") == 0
	      || strcmp (node->Tag, "LineString") == 0)
	      ;
	  else
	      goto error;
	  *next = node->Next;
	  goto ok;
      }
    goto error;

  ok:
/* ok, GML nodes match as expected */
    points = gml_count_dyn_points (dyn);
    if (points < 2)
	goto error;
    if (has_z)
      {
	  ln = gaiaAllocGeomCollXYZ ();
	  ln->Srid = srid;
	  new_ln = gaiaAddLinestringToGeomColl (ln, points);
	  pt = dyn->First;
	  iv = 0;
	  while (pt)
	    {
		gaiaSetPointXYZ (new_ln->Coords, iv, pt->X, pt->Y, pt->Z);
		iv++;
		pt = pt->Next;
	    }
      }
    else
      {
	  ln = gaiaAllocGeomColl ();
	  ln->Srid = srid;
	  new_ln = gaiaAddLinestringToGeomColl (ln, points);
	  pt = dyn->First;
	  iv = 0;
	  while (pt)
	    {
		gaiaSetPoint (new_ln->Coords, iv, pt->X, pt->Y);
		iv++;
		pt = pt->Next;
	    }
      }
    last = geom;
    while (1)
      {
	  /* searching the last Geometry within chain */
	  if (last->Next == NULL)
	      break;
	  last = last->Next;
      }
    last->Next = ln;
    gaiaFreeDynamicLine (dyn);
    return 1;

  error:
    gaiaFreeDynamicLine (dyn);
    return 0;
}

static int
gml_parse_curve (gaiaGeomCollPtr geom, gmlNodePtr node, int srid,
		 gmlNodePtr * next)
{
/* parsing a <gml:Curve> */
    gaiaGeomCollPtr ln;
    gaiaGeomCollPtr last;
    gaiaLinestringPtr new_ln;
    gaiaPointPtr pt;
    gaiaDynamicLinePtr dyn = gaiaAllocDynamicLine ();
    int iv;
    int has_z = 1;
    int points = 0;

    if (strcmp (node->Tag, "gml:segments") == 0
	|| strcmp (node->Tag, "segments") == 0)
      {
	  /* parsing a GML v.3.x <gml:Curve> */
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LineStringSegment") == 0
	      || strcmp (node->Tag, "LineStringSegment") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  has_z = gml_get_srsDimension (node);
	  if (!gml_parse_posList (node->Coordinates, dyn, has_z))
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LineStringSegment") == 0
	      || strcmp (node->Tag, "LineStringSegment") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:segments") == 0
	      || strcmp (node->Tag, "segments") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:Curve") == 0
	      || strcmp (node->Tag, "Curve") == 0)
	      ;
	  else
	      goto error;
	  *next = node->Next;
	  goto ok;
      }
    goto error;

  ok:
/* ok, GML nodes match as expected */
    points = gml_count_dyn_points (dyn);
    if (points < 2)
	goto error;
    if (has_z)
      {
	  ln = gaiaAllocGeomCollXYZ ();
	  ln->Srid = srid;
	  new_ln = gaiaAddLinestringToGeomColl (ln, points);
	  pt = dyn->First;
	  iv = 0;
	  while (pt)
	    {
		gaiaSetPointXYZ (new_ln->Coords, iv, pt->X, pt->Y, pt->Z);
		iv++;
		pt = pt->Next;
	    }
      }
    else
      {
	  ln = gaiaAllocGeomColl ();
	  ln->Srid = srid;
	  new_ln = gaiaAddLinestringToGeomColl (ln, points);
	  pt = dyn->First;
	  iv = 0;
	  while (pt)
	    {
		gaiaSetPoint (new_ln->Coords, iv, pt->X, pt->Y);
		iv++;
		pt = pt->Next;
	    }
      }
    last = geom;
    while (1)
      {
	  /* searching the last Geometry within chain */
	  if (last->Next == NULL)
	      break;
	  last = last->Next;
      }
    last->Next = ln;
    gaiaFreeDynamicLine (dyn);
    return 1;

  error:
    gaiaFreeDynamicLine (dyn);
    return 0;
}

static gaiaDynamicLinePtr
gml_parse_ring (gmlNodePtr node, int *interior, int *has_z, gmlNodePtr * next)
{
/* parsing a generic GML ring */
    gaiaDynamicLinePtr dyn = gaiaAllocDynamicLine ();
    *has_z = 1;

    if (strcmp (node->Tag, "gml:outerBoundaryIs") == 0
	|| strcmp (node->Tag, "outerBoundaryIs") == 0)
      {
	  /* parsing a GML v.2.x <gml:outerBoundaryIs> */
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:coordinates") == 0
	      || strcmp (node->Tag, "coordinates") == 0)
	    {
		/* parsing a GML v.2.x <gml:coordinates> */
		if (!gml_parse_coordinates (node->Coordinates, dyn, has_z))
		    goto error;
		node = node->Next;
		if (node == NULL)
		    goto error;
		if (strcmp (node->Tag, "gml:coordinates") == 0
		    || strcmp (node->Tag, "coordinates") == 0)
		    ;
		else
		    goto error;
	    }
	  else if (strcmp (node->Tag, "gml:posList") == 0
		   || strcmp (node->Tag, "posList") == 0)
	    {
		/* parsing a GML v.3.x <gml:posList> */
		*has_z = gml_get_srsDimension (node);
		if (!gml_parse_posList (node->Coordinates, dyn, *has_z))
		    goto error;
		node = node->Next;
		if (node == NULL)
		    goto error;
		if (strcmp (node->Tag, "gml:posList") == 0
		    || strcmp (node->Tag, "posList") == 0)
		    ;
		else
		    goto error;
	    }
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:outerBoundaryIs") == 0
	      || strcmp (node->Tag, "outerBoundaryIs") == 0)
	      ;
	  else
	      goto error;
	  *interior = 0;
	  *next = node->Next;
	  return dyn;
      }
    if (strcmp (node->Tag, "gml:innerBoundaryIs") == 0
	|| strcmp (node->Tag, "innerBoundaryIs") == 0)
      {
	  /* parsing a GML v.2.x <gml:innerBoundaryIs> */
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:coordinates") == 0
	      || strcmp (node->Tag, "coordinates") == 0)
	    {
		/* parsing a GML v.2.x <gml:coordinates> */
		if (!gml_parse_coordinates (node->Coordinates, dyn, has_z))
		    goto error;
		node = node->Next;
		if (node == NULL)
		    goto error;
		if (strcmp (node->Tag, "gml:coordinates") == 0
		    || strcmp (node->Tag, "coordinates") == 0)
		    ;
		else
		    goto error;
	    }
	  else if (strcmp (node->Tag, "gml:posList") == 0
		   || strcmp (node->Tag, "posList") == 0)
	    {
		/* parsing a GML v.3.x <gml:posList> */
		*has_z = gml_get_srsDimension (node);
		if (!gml_parse_posList (node->Coordinates, dyn, *has_z))
		    goto error;
		node = node->Next;
		if (node == NULL)
		    goto error;
		if (strcmp (node->Tag, "gml:posList") == 0
		    || strcmp (node->Tag, "posList") == 0)
		    ;
		else
		    goto error;
	    }
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:innerBoundaryIs") == 0
	      || strcmp (node->Tag, "innerBoundaryIs") == 0)
	      ;
	  else
	      goto error;
	  *interior = 1;
	  *next = node->Next;
	  return dyn;
      }
    if (strcmp (node->Tag, "gml:exterior") == 0
	|| strcmp (node->Tag, "exterior") == 0)
      {
	  /* parsing a GML v.3.x <gml:exterior> */
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  *has_z = gml_get_srsDimension (node);
	  if (!gml_parse_posList (node->Coordinates, dyn, *has_z))
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:exterior") == 0
	      || strcmp (node->Tag, "exterior") == 0)
	      ;
	  else
	      goto error;
	  *interior = 0;
	  *next = node->Next;
	  return dyn;
      }
    if (strcmp (node->Tag, "gml:interior") == 0
	|| strcmp (node->Tag, "interior") == 0)
      {
	  /* parsing a GML v.3.x <gml:interior> */
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  *has_z = gml_get_srsDimension (node);
	  if (!gml_parse_posList (node->Coordinates, dyn, *has_z))
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:posList") == 0
	      || strcmp (node->Tag, "posList") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:LinearRing") == 0
	      || strcmp (node->Tag, "LinearRing") == 0)
	      ;
	  else
	      goto error;
	  node = node->Next;
	  if (node == NULL)
	      goto error;
	  if (strcmp (node->Tag, "gml:interior") == 0
	      || strcmp (node->Tag, "interior") == 0)
	      ;
	  else
	      goto error;
	  *interior = 1;
	  *next = node->Next;
	  return dyn;
      }

  error:
    gaiaFreeDynamicLine (dyn);
    return 0;
}

static int
gml_parse_polygon (gaiaGeomCollPtr geom, gmlNodePtr node, int srid,
		   gmlNodePtr * next_n)
{
/* parsing a <gml:Polygon> */
    int interior;
    int has_z;
    int inners;
    int outers;
    int points;
    int iv;
    int ib = 0;
    gaiaGeomCollPtr pg;
    gaiaGeomCollPtr last_g;
    gaiaPolygonPtr new_pg;
    gaiaRingPtr ring;
    gaiaDynamicLinePtr dyn;
    gaiaPointPtr pt;
    gaiaDynamicLinePtr exterior_ring;
    gmlNodePtr next;
    gmlDynamicRingPtr dyn_rng;
    gmlDynamicPolygonPtr dyn_pg = gml_alloc_dyn_polygon ();
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on rings */
	  if (strcmp (n->Tag, "gml:Polygon") == 0
	      || strcmp (n->Tag, "Polygon") == 0)
	    {
		*next_n = n->Next;
		break;
	    }
	  dyn = gml_parse_ring (n, &interior, &has_z, &next);
	  if (dyn == NULL)
	      goto error;
	  if (gml_count_dyn_points (dyn) < 4)
	    {
		/* cannot be a valid ring */
		goto error;
	    }
	  /* checking if the ring is closed */
	  if (has_z)
	    {
		if (dyn->First->X == dyn->Last->X
		    && dyn->First->Y == dyn->Last->Y
		    && dyn->First->Z == dyn->Last->Z)
		    ;
		else
		    goto error;
	    }
	  else
	    {
		if (dyn->First->X == dyn->Last->X
		    && dyn->First->Y == dyn->Last->Y)
		    ;
		else
		    goto error;
	    }
	  gml_add_polygon_ring (dyn_pg, dyn, interior, has_z);
	  n = next;
      }
/* ok, GML nodes match as expected */
    inners = 0;
    outers = 0;
    has_z = 1;
    dyn_rng = dyn_pg->first;
    while (dyn_rng)
      {
	  /* verifying the rings collection */
	  if (dyn_rng->has_z == 0)
	      has_z = 0;
	  if (dyn_rng->interior)
	      inners++;
	  else
	    {
		outers++;
		points = gml_count_dyn_points (dyn_rng->ring);
		exterior_ring = dyn_rng->ring;
	    }
	  dyn_rng = dyn_rng->next;
      }
    if (outers != 1)		/* no exterior ring declared */
	goto error;

    if (has_z)
      {
	  pg = gaiaAllocGeomCollXYZ ();
	  pg->Srid = srid;
	  new_pg = gaiaAddPolygonToGeomColl (pg, points, inners);
	  /* initializing the EXTERIOR RING */
	  ring = new_pg->Exterior;
	  pt = exterior_ring->First;
	  iv = 0;
	  while (pt)
	    {
		gaiaSetPointXYZ (ring->Coords, iv, pt->X, pt->Y, pt->Z);
		iv++;
		pt = pt->Next;
	    }
	  dyn_rng = dyn_pg->first;
	  while (dyn_rng)
	    {
		/* initializing any INTERIOR RING */
		if (dyn_rng->interior == 0)
		  {
		      dyn_rng = dyn_rng->next;
		      continue;
		  }
		points = gml_count_dyn_points (dyn_rng->ring);
		ring = gaiaAddInteriorRing (new_pg, ib, points);
		ib++;
		pt = dyn_rng->ring->First;
		iv = 0;
		while (pt)
		  {
		      gaiaSetPointXYZ (ring->Coords, iv, pt->X, pt->Y, pt->Z);
		      iv++;
		      pt = pt->Next;
		  }
		dyn_rng = dyn_rng->next;
	    }
      }
    else
      {
	  pg = gaiaAllocGeomColl ();
	  pg->Srid = srid;
	  new_pg = gaiaAddPolygonToGeomColl (pg, points, inners);
	  /* initializing the EXTERIOR RING */
	  ring = new_pg->Exterior;
	  pt = exterior_ring->First;
	  iv = 0;
	  while (pt)
	    {
		gaiaSetPoint (ring->Coords, iv, pt->X, pt->Y);
		iv++;
		pt = pt->Next;
	    }
	  dyn_rng = dyn_pg->first;
	  while (dyn_rng)
	    {
		/* initializing any INTERIOR RING */
		if (dyn_rng->interior == 0)
		  {
		      dyn_rng = dyn_rng->next;
		      continue;
		  }
		points = gml_count_dyn_points (dyn_rng->ring);
		ring = gaiaAddInteriorRing (new_pg, ib, points);
		ib++;
		pt = dyn_rng->ring->First;
		iv = 0;
		while (pt)
		  {
		      gaiaSetPoint (ring->Coords, iv, pt->X, pt->Y);
		      iv++;
		      pt = pt->Next;
		  }
		dyn_rng = dyn_rng->next;
	    }
      }

    last_g = geom;
    while (1)
      {
	  /* searching the last Geometry within chain */
	  if (last_g->Next == NULL)
	      break;
	  last_g = last_g->Next;
      }
    last_g->Next = pg;
    gml_free_dyn_polygon (dyn_pg);
    return 1;

  error:
    gml_free_dyn_polygon (dyn_pg);
    return 0;
}

static int
gml_parse_multi_point (gaiaGeomCollPtr geom, gmlNodePtr node)
{
/* parsing a <gml:MultiPoint> */
    int srid;
    gmlNodePtr next;
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on Point Members */
	  if (n->Next == NULL)
	    {
		/* verifying the last GML node */
		if (strcmp (n->Tag, "gml:MultiPoint") == 0
		    || strcmp (n->Tag, "MultiPoint") == 0)
		    break;
		else
		    return 0;
	    }
	  if (strcmp (n->Tag, "gml:pointMember") == 0
	      || strcmp (n->Tag, "pointMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:Point") == 0
	      || strcmp (n->Tag, "Point") == 0)
	      ;
	  else
	      return 0;
	  srid = guessGmlSrid (n);
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (!gml_parse_point (geom, n, srid, &next))
	      return 0;
	  n = next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:pointMember") == 0
	      || strcmp (n->Tag, "pointMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
      }
    return 1;
}

static int
gml_parse_multi_linestring (gaiaGeomCollPtr geom, gmlNodePtr node)
{
/* parsing a <gml:MultiLineString> */
    int srid;
    gmlNodePtr next;
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on LineString Members */
	  if (n->Next == NULL)
	    {
		/* verifying the last GML node */
		if (strcmp (n->Tag, "gml:MultiLineString") == 0
		    || strcmp (n->Tag, "MultiLineString") == 0)
		    break;
		else
		    return 0;
	    }
	  if (strcmp (n->Tag, "gml:lineStringMember") == 0
	      || strcmp (n->Tag, "lineStringMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:LineString") == 0
	      || strcmp (n->Tag, "LineString") == 0)
	      ;
	  else
	      return 0;
	  srid = guessGmlSrid (n);
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (!gml_parse_linestring (geom, n, srid, &next))
	      return 0;
	  n = next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:lineStringMember") == 0
	      || strcmp (n->Tag, "lineStringMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
      }
    return 1;
}

static int
gml_parse_multi_curve (gaiaGeomCollPtr geom, gmlNodePtr node)
{
/* parsing a <gml:MultiCurve> */
    int srid;
    gmlNodePtr next;
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on Curve Members */
	  if (n->Next == NULL)
	    {
		/* verifying the last GML node */
		if (strcmp (n->Tag, "gml:MultiCurve") == 0
		    || strcmp (n->Tag, "MultiCurve") == 0)
		    break;
		else
		    return 0;
	    }
	  if (strcmp (n->Tag, "gml:curveMember") == 0
	      || strcmp (n->Tag, "curveMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:Curve") == 0
	      || strcmp (n->Tag, "Curve") == 0)
	      ;
	  else
	      return 0;
	  srid = guessGmlSrid (n);
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (!gml_parse_curve (geom, n, srid, &next))
	      return 0;
	  n = next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:curveMember") == 0
	      || strcmp (n->Tag, "curveMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
      }
    return 1;
}

static int
gml_parse_multi_polygon (gaiaGeomCollPtr geom, gmlNodePtr node)
{
/* parsing a <gml:MultiPolygon> */
    int srid;
    gmlNodePtr next;
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on Polygon Members */
	  if (n->Next == NULL)
	    {
		/* verifying the last GML node */
		if (strcmp (n->Tag, "gml:MultiPolygon") == 0
		    || strcmp (n->Tag, "MultiPolygon") == 0)
		    break;
		else
		    return 0;
	    }
	  if (strcmp (n->Tag, "gml:polygonMember") == 0
	      || strcmp (n->Tag, "polygonMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:Polygon") == 0
	      || strcmp (n->Tag, "Polygon") == 0)
	      ;
	  else
	      return 0;
	  srid = guessGmlSrid (n);
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (!gml_parse_polygon (geom, n, srid, &next))
	      return 0;
	  n = next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:polygonMember") == 0
	      || strcmp (n->Tag, "polygonMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
      }
    return 1;
}

static int
gml_parse_multi_surface (gaiaGeomCollPtr geom, gmlNodePtr node)
{
/* parsing a <gml:MultiSurface> */
    int srid;
    gmlNodePtr next;
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on Surface Members */
	  if (n->Next == NULL)
	    {
		/* verifying the last GML node */
		if (strcmp (n->Tag, "gml:MultiSurface") == 0
		    || strcmp (n->Tag, "MultiSurface") == 0)
		    break;
		else
		    return 0;
	    }
	  if (strcmp (n->Tag, "gml:surfaceMember") == 0
	      || strcmp (n->Tag, "surfaceMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:Polygon") == 0
	      || strcmp (n->Tag, "Polygon") == 0)
	      ;
	  else
	      return 0;
	  srid = guessGmlSrid (n);
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (!gml_parse_polygon (geom, n, srid, &next))
	      return 0;
	  n = next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:surfaceMember") == 0
	      || strcmp (n->Tag, "surfaceMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
      }
    return 1;
}

static int
gml_parse_multi_geometry (gaiaGeomCollPtr geom, gmlNodePtr node)
{
/* parsing a <gml:MultiGeometry> */
    int srid;
    gmlNodePtr next;
    gmlNodePtr n = node;
    while (n)
      {
	  /* looping on Geometry Members */
	  if (n->Next == NULL)
	    {
		/* verifying the last GML node */
		if (strcmp (n->Tag, "gml:MultiGeometry") == 0
		    || strcmp (n->Tag, "MultiGeometry") == 0)
		    break;
		else
		    return 0;
	    }
	  if (strcmp (n->Tag, "gml:geometryMember") == 0
	      || strcmp (n->Tag, "geometryMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:Point") == 0
	      || strcmp (n->Tag, "Point") == 0)
	    {
		srid = guessGmlSrid (n);
		n = n->Next;
		if (n == NULL)
		    return 0;
		if (!gml_parse_point (geom, n, srid, &next))
		    return 0;
		n = next;
	    }
	  else if (strcmp (n->Tag, "gml:LineString") == 0
		   || strcmp (n->Tag, "LineString") == 0)
	    {
		srid = guessGmlSrid (n);
		n = n->Next;
		if (n == NULL)
		    return 0;
		if (!gml_parse_linestring (geom, n, srid, &next))
		    return 0;
		n = next;
	    }
	  else if (strcmp (n->Tag, "gml:Curve") == 0
		   || strcmp (n->Tag, "Curve") == 0)
	    {
		srid = guessGmlSrid (n);
		n = n->Next;
		if (n == NULL)
		    return 0;
		if (!gml_parse_curve (geom, n, srid, &next))
		    return 0;
		n = next;
	    }
	  else if (strcmp (n->Tag, "gml:Polygon") == 0
		   || strcmp (n->Tag, "Polygon") == 0)
	    {
		srid = guessGmlSrid (n);
		n = n->Next;
		if (n == NULL)
		    return 0;
		if (!gml_parse_polygon (geom, n, srid, &next))
		    return 0;
		n = next;
	    }
	  else
	      return 0;
	  if (n == NULL)
	      return 0;
	  if (strcmp (n->Tag, "gml:geometryMember") == 0
	      || strcmp (n->Tag, "geometryMember") == 0)
	      ;
	  else
	      return 0;
	  n = n->Next;
      }
    return 1;
}

static gaiaGeomCollPtr
gml_validate_geometry (gaiaGeomCollPtr chain, sqlite3 * sqlite_handle)
{
    int xy = 0;
    int xyz = 0;
    int pts = 0;
    int lns = 0;
    int pgs = 0;
    gaiaPointPtr pt;
    gaiaLinestringPtr ln;
    gaiaPolygonPtr pg;
    gaiaPointPtr save_pt;
    gaiaLinestringPtr save_ln;
    gaiaPolygonPtr save_pg;
    gaiaRingPtr i_ring;
    gaiaRingPtr o_ring;
    int ib;
    int delete_g2;
    gaiaGeomCollPtr g;
    gaiaGeomCollPtr g2;
    gaiaGeomCollPtr geom;
    char proj_from[2048];
    char proj_to[2048];

    g = chain;
    while (g)
      {
	  if (g != chain)
	    {
		if (g->DimensionModel == GAIA_XY)
		    xy++;
		if (g->DimensionModel == GAIA_XY_Z)
		    xyz++;
	    }
	  pt = g->FirstPoint;
	  while (pt)
	    {
		pts++;
		save_pt = pt;
		pt = pt->Next;
	    }
	  ln = g->FirstLinestring;
	  while (ln)
	    {
		lns++;
		save_ln = ln;
		ln = ln->Next;
	    }
	  pg = g->FirstPolygon;
	  while (pg)
	    {
		pgs++;
		save_pg = pg;
		pg = pg->Next;
	    }
	  g = g->Next;
      }
    if (pts == 1 && lns == 0 && pgs == 0)
      {
	  /* POINT */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_MULTIPOINT)
		    geom->DeclaredType = GAIA_MULTIPOINT;
		else if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_POINT;
		gaiaAddPointToGeomColl (geom, save_pt->X, save_pt->Y);
		return geom;
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_MULTIPOINT)
		    geom->DeclaredType = GAIA_MULTIPOINT;
		else if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_POINT;
		gaiaAddPointToGeomCollXYZ (geom, save_pt->X, save_pt->Y,
					   save_pt->Z);
		return geom;
	    }
      }
    if (pts == 0 && lns == 1 && pgs == 0)
      {
	  /* LINESTRING */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
	    }
	  geom->Srid = chain->Srid;
	  if (chain->DeclaredType == GAIA_MULTILINESTRING)
	      geom->DeclaredType = GAIA_MULTILINESTRING;
	  else if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
	      geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
	  else
	      geom->DeclaredType = GAIA_LINESTRING;
	  ln = gaiaAddLinestringToGeomColl (geom, save_ln->Points);
	  gaiaCopyLinestringCoords (ln, save_ln);
	  return geom;
      }
    if (pts == 0 && lns == 0 && pgs == 1)
      {
	  /* POLYGON */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
	    }
	  geom->Srid = chain->Srid;
	  if (chain->DeclaredType == GAIA_MULTIPOLYGON)
	      geom->DeclaredType = GAIA_MULTIPOLYGON;
	  else if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
	      geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
	  else
	      geom->DeclaredType = GAIA_POLYGON;
	  i_ring = save_pg->Exterior;
	  pg = gaiaAddPolygonToGeomColl (geom, i_ring->Points,
					 save_pg->NumInteriors);
	  o_ring = pg->Exterior;
	  gaiaCopyRingCoords (o_ring, i_ring);
	  for (ib = 0; ib < save_pg->NumInteriors; ib++)
	    {
		i_ring = save_pg->Interiors + ib;
		o_ring = gaiaAddInteriorRing (pg, ib, i_ring->Points);
		gaiaCopyRingCoords (o_ring, i_ring);
	    }
	  return geom;
      }
    if (pts >= 1 && lns == 0 && pgs == 0)
      {
	  /* MULTIPOINT */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_MULTIPOINT;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet set any SRID */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      pt = g2->FirstPoint;
		      while (pt)
			{
			    gaiaAddPointToGeomColl (geom, pt->X, pt->Y);
			    pt = pt->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_MULTIPOINT;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet a SRID set */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      pt = g2->FirstPoint;
		      while (pt)
			{
			    gaiaAddPointToGeomCollXYZ (geom, pt->X, pt->Y,
						       pt->Z);
			    pt = pt->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
      }
    if (pts == 0 && lns >= 1 && pgs == 0)
      {
	  /* MULTILINESTRING */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_MULTILINESTRING;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet set any SRID */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      ln = g2->FirstLinestring;
		      while (ln)
			{
			    save_ln =
				gaiaAddLinestringToGeomColl (geom, ln->Points);
			    gaiaCopyLinestringCoords (save_ln, ln);
			    ln = ln->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_MULTILINESTRING;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet a SRID set */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      ln = g2->FirstLinestring;
		      while (ln)
			{
			    save_ln =
				gaiaAddLinestringToGeomColl (geom, ln->Points);
			    gaiaCopyLinestringCoords (save_ln, ln);
			    ln = ln->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
      }
    if (pts == 0 && lns == 0 && pgs >= 1)
      {
	  /* MULTIPOLYGON */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_MULTIPOLYGON;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet set any SRID */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      pg = g2->FirstPolygon;
		      while (pg)
			{
			    i_ring = pg->Exterior;
			    save_pg =
				gaiaAddPolygonToGeomColl (geom, i_ring->Points,
							  pg->NumInteriors);
			    o_ring = save_pg->Exterior;
			    gaiaCopyRingCoords (o_ring, i_ring);
			    for (ib = 0; ib < pg->NumInteriors; ib++)
			      {
				  i_ring = pg->Interiors + ib;
				  o_ring =
				      gaiaAddInteriorRing (save_pg, ib,
							   i_ring->Points);
				  gaiaCopyRingCoords (o_ring, i_ring);
			      }
			    pg = pg->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
		geom->Srid = chain->Srid;
		if (chain->DeclaredType == GAIA_GEOMETRYCOLLECTION)
		    geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		else
		    geom->DeclaredType = GAIA_MULTIPOLYGON;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet a SRID set */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      pg = g2->FirstPolygon;
		      while (pg)
			{
			    i_ring = pg->Exterior;
			    save_pg =
				gaiaAddPolygonToGeomColl (geom, i_ring->Points,
							  pg->NumInteriors);
			    o_ring = save_pg->Exterior;
			    gaiaCopyRingCoords (o_ring, i_ring);
			    for (ib = 0; ib < pg->NumInteriors; ib++)
			      {
				  i_ring = pg->Interiors + ib;
				  o_ring =
				      gaiaAddInteriorRing (save_pg, ib,
							   i_ring->Points);
				  gaiaCopyRingCoords (o_ring, i_ring);
			      }
			    pg = pg->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
      }
    if ((pts + lns + pgs) > 0)
      {
	  /* GEOMETRYCOLLECTION */
	  if (xy > 0)
	    {
		/* 2D [XY] */
		geom = gaiaAllocGeomColl ();
		geom->Srid = chain->Srid;
		geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet set any SRID */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      pt = g2->FirstPoint;
		      while (pt)
			{
			    gaiaAddPointToGeomColl (geom, pt->X, pt->Y);
			    pt = pt->Next;
			}
		      ln = g2->FirstLinestring;
		      while (ln)
			{
			    save_ln =
				gaiaAddLinestringToGeomColl (geom, ln->Points);
			    gaiaCopyLinestringCoords (save_ln, ln);
			    ln = ln->Next;
			}
		      pg = g2->FirstPolygon;
		      while (pg)
			{
			    i_ring = pg->Exterior;
			    save_pg =
				gaiaAddPolygonToGeomColl (geom, i_ring->Points,
							  pg->NumInteriors);
			    o_ring = save_pg->Exterior;
			    gaiaCopyRingCoords (o_ring, i_ring);
			    for (ib = 0; ib < pg->NumInteriors; ib++)
			      {
				  i_ring = pg->Interiors + ib;
				  o_ring =
				      gaiaAddInteriorRing (save_pg, ib,
							   i_ring->Points);
				  gaiaCopyRingCoords (o_ring, i_ring);
			      }
			    pg = pg->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
	  else
	    {
		/* 3D [XYZ] */
		geom = gaiaAllocGeomCollXYZ ();
		geom->Srid = chain->Srid;
		geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
		g = chain;
		while (g)
		  {
		      if (geom->Srid == -1)
			{
			    /* we haven't yet a SRID set */
			    geom->Srid = g->Srid;
			}
		      g2 = g;
		      delete_g2 = 0;
		      if (g->Srid != geom->Srid && g->Srid != -1
			  && sqlite_handle != NULL)
			{
			    /* we'll try to apply a reprojection */
#ifndef OMIT_PROJ		/* but only if PROJ.4 is actually available */
			    gml_proj_params (sqlite_handle, g->Srid, proj_from);
			    gml_proj_params (sqlite_handle, geom->Srid,
					     proj_to);
			    if (*proj_to == '\0' || *proj_from == '\0')
				;
			    else
			      {
				  g2 = gaiaTransform (g, proj_from, proj_to);
				  if (!g2)
				      g2 = g;
				  else
				      delete_g2 = 1;
			      }
#endif
			}
		      pt = g2->FirstPoint;
		      while (pt)
			{
			    gaiaAddPointToGeomCollXYZ (geom, pt->X, pt->Y,
						       pt->Z);
			    pt = pt->Next;
			}
		      ln = g2->FirstLinestring;
		      while (ln)
			{
			    save_ln =
				gaiaAddLinestringToGeomColl (geom, ln->Points);
			    gaiaCopyLinestringCoords (save_ln, ln);
			    ln = ln->Next;
			}
		      pg = g2->FirstPolygon;
		      while (pg)
			{
			    i_ring = pg->Exterior;
			    save_pg =
				gaiaAddPolygonToGeomColl (geom, i_ring->Points,
							  pg->NumInteriors);
			    o_ring = save_pg->Exterior;
			    gaiaCopyRingCoords (o_ring, i_ring);
			    for (ib = 0; ib < pg->NumInteriors; ib++)
			      {
				  i_ring = pg->Interiors + ib;
				  o_ring =
				      gaiaAddInteriorRing (save_pg, ib,
							   i_ring->Points);
				  gaiaCopyRingCoords (o_ring, i_ring);
			      }
			    pg = pg->Next;
			}
		      if (delete_g2)
			  gaiaFreeGeomColl (g2);
		      g = g->Next;
		  }
		return geom;
	    }
      }
    return NULL;
}

static void
gml_free_geom_chain (gaiaGeomCollPtr geom)
{
/* deleting a chain of preliminary geometries */
    gaiaGeomCollPtr gn;
    while (geom)
      {
	  gn = geom->Next;
	  gaiaFreeGeomColl (geom);
	  geom = gn;
      }
}

static gaiaGeomCollPtr
gml_build_geometry (gmlNodePtr tree, sqlite3 * sqlite_handle)
{
/* attempting to build a geometry from GML nodes */
    gaiaGeomCollPtr geom;
    gaiaGeomCollPtr result;
    int geom_type;
    gmlNodePtr next;

    if (tree == NULL)
	return NULL;
    geom_type = guessGmlGeometryType (tree);
    if (geom_type == GAIA_GML_UNKNOWN)
      {
	  /* unsupported main geometry type */
	  return NULL;
      }
/* creating the main geometry */
    geom = gaiaAllocGeomColl ();
    geom->Srid = guessGmlSrid (tree);

    switch (geom_type)
      {
	  /* parsing GML nodes accordingly with declared GML type */
      case GAIA_GML_POINT:
	  geom->DeclaredType = GAIA_POINT;
	  if (!gml_parse_point (geom, tree->Next, geom->Srid, &next))
	      goto error;
	  break;
      case GAIA_GML_LINESTRING:
	  geom->DeclaredType = GAIA_LINESTRING;
	  if (!gml_parse_linestring (geom, tree->Next, geom->Srid, &next))
	      goto error;
	  break;
      case GAIA_GML_CURVE:
	  geom->DeclaredType = GAIA_LINESTRING;
	  if (!gml_parse_curve (geom, tree->Next, geom->Srid, &next))
	      goto error;
	  break;
      case GAIA_GML_POLYGON:
	  geom->DeclaredType = GAIA_POLYGON;
	  if (!gml_parse_polygon (geom, tree->Next, geom->Srid, &next))
	      goto error;
	  if (next != NULL)
	      goto error;
	  break;
      case GAIA_GML_MULTIPOINT:
	  geom->DeclaredType = GAIA_MULTIPOINT;
	  if (!gml_parse_multi_point (geom, tree->Next))
	      goto error;
	  break;
      case GAIA_GML_MULTILINESTRING:
	  geom->DeclaredType = GAIA_MULTILINESTRING;
	  if (!gml_parse_multi_linestring (geom, tree->Next))
	      goto error;
	  break;
      case GAIA_GML_MULTICURVE:
	  geom->DeclaredType = GAIA_MULTILINESTRING;
	  if (!gml_parse_multi_curve (geom, tree->Next))
	      goto error;
	  break;
      case GAIA_GML_MULTIPOLYGON:
	  geom->DeclaredType = GAIA_MULTIPOLYGON;
	  if (!gml_parse_multi_polygon (geom, tree->Next))
	      goto error;
	  break;
      case GAIA_GML_MULTISURFACE:
	  geom->DeclaredType = GAIA_MULTIPOLYGON;
	  if (!gml_parse_multi_surface (geom, tree->Next))
	      goto error;
	  break;
      case GAIA_GML_MULTIGEOMETRY:
	  geom->DeclaredType = GAIA_GEOMETRYCOLLECTION;
	  if (!gml_parse_multi_geometry (geom, tree->Next))
	      goto error;
	  break;
      };

/* attempting to build the final geometry */
    result = gml_validate_geometry (geom, sqlite_handle);
    if (result == NULL)
	goto error;
    gml_free_geom_chain (geom);
    return result;

  error:
    gml_free_geom_chain (geom);
    return NULL;
}



/*
** CAVEAT: we must redefine any Lemon/Flex own macro
*/
#define YYMINORTYPE		GML_MINORTYPE
#define YY_CHAR			GML_YY_CHAR
#define	input			gml_input
#define ParseAlloc		gmlParseAlloc
#define ParseFree		gmlParseFree
#define ParseStackPeak		gmlParseStackPeak
#define Parse			gmlParse
#define yyStackEntry		gml_yyStackEntry
#define yyzerominor		gml_yyzerominor
#define yy_accept		gml_yy_accept
#define yy_action		gml_yy_action
#define yy_base			gml_yy_base
#define yy_buffer_stack		gml_yy_buffer_stack
#define yy_buffer_stack_max	gml_yy_buffer_stack_max
#define yy_buffer_stack_top	gml_yy_buffer_stack_top
#define yy_c_buf_p		gml_yy_c_buf_p
#define yy_chk			gml_yy_chk
#define yy_def			gml_yy_def
#define yy_default		gml_yy_default
#define yy_destructor		gml_yy_destructor
#define yy_ec			gml_yy_ec
#define yy_fatal_error		gml_yy_fatal_error
#define yy_find_reduce_action	gml_yy_find_reduce_action
#define yy_find_shift_action	gml_yy_find_shift_action
#define yy_get_next_buffer	gml_yy_get_next_buffer
#define yy_get_previous_state	gml_yy_get_previous_state
#define yy_init			gml_yy_init
#define yy_init_globals		gml_yy_init_globals
#define yy_lookahead		gml_yy_lookahead
#define yy_meta			gml_yy_meta
#define yy_nxt			gml_yy_nxt
#define yy_parse_failed		gml_yy_parse_failed
#define yy_pop_parser_stack	gml_yy_pop_parser_stack
#define yy_reduce		gml_yy_reduce
#define yy_reduce_ofst		gml_yy_reduce_ofst
#define yy_shift		gml_yy_shift
#define yy_shift_ofst		gml_yy_shift_ofst
#define yy_start		gml_yy_start
#define yy_state_type		gml_yy_state_type
#define yy_syntax_error		gml_yy_syntax_error
#define yy_trans_info		gml_yy_trans_info
#define yy_try_NUL_trans	gml_yy_try_NUL_trans
#define yyParser		gml_yyParser
#define yyStackEntry		gml_yyStackEntry
#define yyStackOverflow		gml_yyStackOverflow
#define yyRuleInfo		gml_yyRuleInfo
#define yyunput			gml_yyunput
#define yyzerominor		gml_yyzerominor
#define yyTraceFILE		gml_yyTraceFILE
#define yyTracePrompt		gml_yyTracePrompt
#define yyTokenName		gml_yyTokenName
#define yyRuleName		gml_yyRuleName
#define ParseTrace		gml_ParseTrace


/* include LEMON generated header */
#include "Gml.h"


typedef union
{
    char *pval;
    struct symtab *symp;
} gml_yystype;
#define YYSTYPE gml_yystype


/* extern YYSTYPE yylval; */
YYSTYPE GmlLval;



/* including LEMON generated code */
#include "Gml.c"



/*
** CAVEAT: there is an incompatibility between LEMON and FLEX
** this macro resolves the issue
*/
#undef yy_accept
#define yy_accept	yy_gml_flex_accept



/* including FLEX generated code */
#include "lex.Gml.c"



gaiaGeomCollPtr
gaiaParseGml (const unsigned char *dirty_buffer, sqlite3 * sqlite_handle)
{
    void *pParser = ParseAlloc (malloc);
    /* Linked-list of token values */
    gmlFlexToken *tokens = malloc (sizeof (gmlFlexToken));
    /* Pointer to the head of the list */
    gmlFlexToken *head = tokens;
    int yv;
    gmlNodePtr result = NULL;
    gaiaGeomCollPtr geom = NULL;

    GmlLval.pval = NULL;
    tokens->value = NULL;
    tokens->Next = NULL;
    gml_parse_error = 0;
    Gml_scan_string ((char *) dirty_buffer);

    /*
       / Keep tokenizing until we reach the end
       / yylex() will return the next matching Token for us.
     */
    while ((yv = yylex ()) != 0)
      {
	  if (yv == -1)
	    {
		gml_parse_error = 1;
		break;
	    }
	  tokens->Next = malloc (sizeof (gmlFlexToken));
	  tokens->Next->Next = NULL;
	  /*
	     /GmlLval is a global variable from FLEX.
	     /GmlLval is defined in gmlLexglobal.h
	   */
	  gml_xferString (&(tokens->Next->value), GmlLval.pval);
	  /* Pass the token to the wkt parser created from lemon */
	  Parse (pParser, yv, &(tokens->Next->value), &result);
	  tokens = tokens->Next;
      }
    /* This denotes the end of a line as well as the end of the parser */
    Parse (pParser, GML_NEWLINE, 0, &result);
    ParseFree (pParser, free);
    Gmllex_destroy ();

    /* Assigning the token as the end to avoid seg faults while cleaning */
    tokens->Next = NULL;
    gml_cleanup (head);
    gml_freeString (&(GmlLval.pval));

    if (gml_parse_error)
      {
	  if (result)
	      gml_freeTree (result);
	  return NULL;
      }

    /* attempting to build a geometry from GML */
    geom = gml_build_geometry (result, sqlite_handle);
    gml_freeTree (result);
    return geom;
}


/*
** CAVEAT: we must now undefine any Lemon/Flex own macro
*/
#undef YYNOCODE
#undef YYNSTATE
#undef YYNRULE
#undef YY_SHIFT_MAX
#undef YY_SHIFT_USE_DFLT
#undef YY_REDUCE_USE_DFLT
#undef YY_REDUCE_MAX
#undef YY_FLUSH_BUFFER
#undef YY_DO_BEFORE_ACTION
#undef YY_NUM_RULES
#undef YY_END_OF_BUFFER
#undef YY_END_FILE
#undef YYACTIONTYPE
#undef YY_SZ_ACTTAB
#undef YY_NEW_FILE
#undef BEGIN
#undef YY_START
#undef YY_CURRENT_BUFFER
#undef YY_CURRENT_BUFFER_LVALUE
#undef YY_STATE_BUF_SIZE
#undef YY_DECL
#undef YY_FATAL_ERROR
#undef YYMINORTYPE
#undef YY_CHAR
#undef YYSTYPE
#undef input
#undef ParseAlloc
#undef ParseFree
#undef ParseStackPeak
#undef Parse
#undef yyalloc
#undef yyfree
#undef yyin
#undef yyleng
#undef yyless
#undef yylex
#undef yylineno
#undef yyout
#undef yyrealloc
#undef yyrestart
#undef yyStackEntry
#undef yytext
#undef yywrap
#undef yyzerominor
#undef yy_accept
#undef yy_action
#undef yy_base
#undef yy_buffer_stack
#undef yy_buffer_stack_max
#undef yy_buffer_stack_top
#undef yy_c_buf_p
#undef yy_chk
#undef yy_create_buffer
#undef yy_def
#undef yy_default
#undef yy_delete_buffer
#undef yy_destructor
#undef yy_ec
#undef yy_fatal_error
#undef yy_find_reduce_action
#undef yy_find_shift_action
#undef yy_flex_debug
#undef yy_flush_buffer
#undef yy_get_next_buffer
#undef yy_get_previous_state
#undef yy_init
#undef yy_init_buffer
#undef yy_init_globals
#undef yy_load_buffer
#undef yy_load_buffer_state
#undef yy_lookahead
#undef yy_meta
#undef yy_new_buffer
#undef yy_nxt
#undef yy_parse_failed
#undef yy_pop_parser_stack
#undef yy_reduce
#undef yy_reduce_ofst
#undef yy_set_bol
#undef yy_set_interactive
#undef yy_shift
#undef yy_shift_ofst
#undef yy_start
#undef yy_state_type
#undef yy_switch_to_buffer
#undef yy_syntax_error
#undef yy_trans_info
#undef yy_try_NUL_trans
#undef yyParser
#undef yyStackEntry
#undef yyStackOverflow
#undef yyRuleInfo
#undef yytext_ptr
#undef yyunput
#undef yyzerominor
#undef ParseARG_SDECL
#undef ParseARG_PDECL
#undef ParseARG_FETCH
#undef ParseARG_STORE
#undef REJECT
#undef yymore
#undef YY_MORE_ADJ
#undef YY_RESTORE_YY_MORE_OFFSET
#undef YY_LESS_LINENO
#undef yyTracePrompt
#undef yyTraceFILE
#undef yyTokenName
#undef yyRuleName
#undef ParseTrace
