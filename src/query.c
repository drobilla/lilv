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
#include "zix/tree.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
	LILV_LANG_MATCH_NONE,     ///< Language does not match at all
	LILV_LANG_MATCH_PARTIAL,  ///< Partial (language, but not country) match
	LILV_LANG_MATCH_EXACT     ///< Exact (language and country) match
} LilvLangMatch;

static LilvLangMatch
lilv_lang_matches(const char* a, const char* b)
{
	if (!strcmp(a, b)) {
		return LILV_LANG_MATCH_EXACT;
	}

	const char*  a_dash     = strchr(a, '-');
	const size_t a_lang_len = a_dash ? (size_t)(a_dash - a) : strlen(a);
	const char*  b_dash     = strchr(b, '-');
	const size_t b_lang_len = b_dash ? (size_t)(b_dash - b) : strlen(b);

	if (a_lang_len == b_lang_len && !strncmp(a, b, a_lang_len)) {
		return LILV_LANG_MATCH_PARTIAL;
	}

	return LILV_LANG_MATCH_NONE;
}

static LilvNodes*
lilv_nodes_from_range_i18n(LilvWorld* world, SerdRange* range, SerdField field)
{
	LilvNodes*      values  = lilv_nodes_new();
	const SerdNode* nolang  = NULL;  // Untranslated value
	const SerdNode* partial = NULL;  // Partial language match
	char*           syslang = lilv_get_lang();
	FOREACH_MATCH(s, range) {
		const SerdNode* value = serd_statement_get_node(s, field);
		if (serd_node_get_type(value) == SERD_LITERAL) {
			const SerdNode* lang = serd_node_get_language(value);
			LilvLangMatch   lm   = LILV_LANG_MATCH_NONE;
			if (lang) {
				lm = (syslang)
					? lilv_lang_matches(serd_node_get_string(lang), syslang)
					: LILV_LANG_MATCH_PARTIAL;
			} else {
				nolang = value;
				if (!syslang) {
					lm = LILV_LANG_MATCH_EXACT;
				}
			}

			if (lm == LILV_LANG_MATCH_EXACT) {
				// Exact language match, add to results
				zix_tree_insert((ZixTree*)values, serd_node_copy(value), NULL);
			} else if (lm == LILV_LANG_MATCH_PARTIAL) {
				// Partial language match, save in case we find no exact
				partial = value;
			}
		} else {
			zix_tree_insert((ZixTree*)values, serd_node_copy(value), NULL);
		}
	}
	serd_range_free(range);
	free(syslang);

	if (lilv_nodes_size(values) > 0) {
		return values;
	}

	const SerdNode* best = nolang;
	if (syslang && partial) {
		// Partial language match for system language
		best = partial;
	} else if (!best) {
		// No languages matches at all, and no untranslated value
		// Use any value, if possible
		best = partial;
	}

	if (best) {
		zix_tree_insert((ZixTree*)values, serd_node_copy(best), NULL);
	} else {
		// No matches whatsoever
		lilv_nodes_free(values);
		values = NULL;
	}

	return values;
}

LilvNodes*
lilv_nodes_from_range(LilvWorld* world, SerdRange* range, SerdField field)
{
	if (serd_range_empty(range)) {
		serd_range_free(range);
		return NULL;
	} else if (world->opt.filter_language) {
		return lilv_nodes_from_range_i18n(world, range, field);
	} else {
		LilvNodes* values = lilv_nodes_new();
		FOREACH_MATCH(s, range) {
			const SerdNode* value = serd_statement_get_node(s, field);
			LilvNode*       node  = serd_node_copy(value);
			if (node) {
				zix_tree_insert((ZixTree*)values, node, NULL);
			}
		}
		serd_range_free(range);
		return values;
	}
}
