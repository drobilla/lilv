/*
  Copyright 2007-2019 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "lilv_internal.h"

#include "lilv/lilv.h"
#include "serd/serd.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LILV_API LilvNode*
lilv_new_uri(LilvWorld* world, const char* uri)
{
	return serd_new_uri(uri);
}

LILV_API LilvNode*
lilv_new_file_uri(LilvWorld* world, const char* host, const char* path)
{
	char*     abs_path = lilv_path_absolute(path);
	SerdNode* s        = serd_new_file_uri(abs_path, host);

	free(abs_path);
	return s;
}

LILV_API LilvNode*
lilv_new_string(LilvWorld* world, const char* str)
{
	return serd_new_string(str);
}

LILV_API LilvNode*
lilv_new_int(LilvWorld* world, int val)
{
	return serd_new_integer(val, world->uris.xsd_int);
}

LILV_API LilvNode*
lilv_new_float(LilvWorld* world, float val)
{
	return serd_new_float(val);
}

LILV_API LilvNode*
lilv_new_bool(LilvWorld* world, bool val)
{
	return serd_new_typed_literal(val ? "true" : "false",
	                              world->uris.xsd_boolean);
}

LILV_API LilvNode*
lilv_node_duplicate(const LilvNode* val)
{
	return serd_node_copy(val);
}

LILV_API void
lilv_node_free(LilvNode* val)
{
	serd_node_free(val);
}

LILV_API bool
lilv_node_equals(const LilvNode* value, const LilvNode* other)
{
	return serd_node_equals(value, other);
}

LILV_API char*
lilv_node_get_turtle_token(const LilvNode* value)
{
	const char* str    = serd_node_get_string(value);
	size_t      len    = 0;
	char*       result = NULL;

	if (lilv_node_is_uri(value)) {
		len    = strlen(str) + 3;
		result = (char*)calloc(len, 1);
		snprintf(result, len, "<%s>", str);
	} else if (lilv_node_is_blank(value)) {
		len    = strlen(str) + 3;
		result = (char*)calloc(len, 1);
		snprintf(result, len, "_:%s", str);
	} else {
		result = lilv_strdup(str);
	}

	return result;
}

LILV_API bool
lilv_node_is_uri(const LilvNode* value)
{
	return serd_node_get_type(value) == SERD_URI;
}

LILV_API const char*
lilv_node_as_uri(const LilvNode* value)
{
	return (lilv_node_is_uri(value)
	        ? serd_node_get_string(value)
	        : NULL);
}

LILV_API bool
lilv_node_is_blank(const LilvNode* value)
{
	return serd_node_get_type(value) == SERD_BLANK;
}

LILV_API const char*
lilv_node_as_blank(const LilvNode* value)
{
	return (lilv_node_is_blank(value)
	        ? serd_node_get_string(value)
	        : NULL);
}

LILV_API bool
lilv_node_is_literal(const LilvNode* value)
{
	return serd_node_get_type(value) == SERD_LITERAL;
}

LILV_API bool
lilv_node_is_string(const LilvNode* value)
{
	return (serd_node_get_type(value) == SERD_LITERAL &&
	        !serd_node_get_datatype(value));
}

LILV_API const char*
lilv_node_as_string(const LilvNode* value)
{
	return value ? serd_node_get_string(value) : NULL;
}

LILV_API bool
lilv_node_is_int(const LilvNode* value)
{
	const SerdNode* const datatype = serd_node_get_datatype(value);
	return (serd_node_get_type(value) == SERD_LITERAL && datatype &&
	        (!strcmp(serd_node_get_string(datatype), LILV_NS_XSD "integer") ||
	         !strcmp(serd_node_get_string(datatype), LILV_NS_XSD "int")));
}

LILV_API int
lilv_node_as_int(const LilvNode* value)
{
	return lilv_node_is_int(value)
	               ? strtoll(serd_node_get_string(value), NULL, 10)
	               : 0;
}

LILV_API bool
lilv_node_is_float(const LilvNode* value)
{
	const SerdNode* const datatype = serd_node_get_datatype(value);
	return (serd_node_get_type(value) == SERD_LITERAL && datatype &&
	        (!strcmp(serd_node_get_string(datatype), LILV_NS_XSD "decimal") ||
	         !strcmp(serd_node_get_string(datatype), LILV_NS_XSD "float") ||
	         !strcmp(serd_node_get_string(datatype), LILV_NS_XSD "double")));
}

LILV_API float
lilv_node_as_float(const LilvNode* value)
{
	if (lilv_node_is_float(value)) {
		return serd_strtod(serd_node_get_string(value), NULL);
	} else if (lilv_node_is_int(value)) {
		return (float)strtol(serd_node_get_string(value), NULL, 10);
	}
	return NAN;
}

LILV_API bool
lilv_node_is_bool(const LilvNode* value)
{
	const SerdNode* const datatype = serd_node_get_datatype(value);
	return (serd_node_get_type(value) == SERD_LITERAL && datatype &&
	        (!strcmp(serd_node_get_string(datatype), LILV_NS_XSD "boolean")));
}

LILV_API bool
lilv_node_as_bool(const LilvNode* value)
{
	return (lilv_node_is_bool(value) &&
	        !strcmp(serd_node_get_string(value), "true"));
}

LILV_API char*
lilv_node_get_path(const LilvNode* value, char** hostname)
{
	if (lilv_node_is_uri(value)) {
		return lilv_file_uri_parse(lilv_node_as_uri(value), hostname);
	}
	return NULL;
}
