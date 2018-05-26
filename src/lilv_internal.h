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

#ifndef LILV_INTERNAL_H
#define LILV_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lilv_config.h"

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "zix/tree.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#    include <windows.h>
#    include <direct.h>
#    include <stdio.h>
#    define dlopen(path, flags) LoadLibrary(path)
#    define dlclose(lib)        FreeLibrary((HMODULE)lib)
#    define unlink(path)        _unlink(path)
#    define rmdir(path)         _rmdir(path)
#    ifdef _MSC_VER
#        define __func__ __FUNCTION__
#        ifndef snprintf
#            define snprintf _snprintf
#        endif
#    endif
#ifndef INFINITY
#    define INFINITY DBL_MAX + DBL_MAX
#endif
#ifndef NAN
#    define NAN INFINITY - INFINITY
#endif
static inline const char* dlerror(void) { return "Unknown error"; }
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif

#ifdef LILV_DYN_MANIFEST
#    include "lv2/dynmanifest/dynmanifest.h"
#endif

#define LILV_READER_STACK_SIZE 65536

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/*
 *
 * Types
 *
 */

typedef struct LilvSpecImpl LilvSpec;

typedef void LilvCollection;

struct LilvPortImpl {
	LilvNode*  node;     ///< RDF node
	uint32_t   index;    ///< lv2:index
	LilvNode*  symbol;   ///< lv2:symbol
	LilvNodes* classes;  ///< rdf:type
};

struct LilvSpecImpl {
	SerdNode*            spec;
	SerdNode*            bundle;
	LilvNodes*           data_uris;
	struct LilvSpecImpl* next;
};

/**
   Header of an LilvPlugin, LilvPluginClass, or LilvUI.
   Any of these structs may be safely casted to LilvHeader, which is used to
   implement collections using the same comparator.
*/
struct LilvHeader {
	LilvWorld* world;
	LilvNode*  uri;
};

#ifdef LILV_DYN_MANIFEST
typedef struct {
	LilvNode*               bundle;
	void*                   lib;
	LV2_Dyn_Manifest_Handle handle;
	uint32_t                refs;
} LilvDynManifest;
#endif

typedef struct {
	LilvWorld*                world;
	LilvNode*                 uri;
	char*                     bundle_path;
	void*                     lib;
	LV2_Descriptor_Function   lv2_descriptor;
	const LV2_Lib_Descriptor* desc;
	uint32_t                  refs;
} LilvLib;

struct LilvPluginImpl {
	LilvWorld*             world;
	LilvNode*              plugin_uri;
	LilvNode*              bundle_uri;  ///< Bundle plugin was loaded from
	LilvNode*              binary_uri;  ///< lv2:binary
#ifdef LILV_DYN_MANIFEST
	LilvDynManifest*       dynmanifest;
#endif
	const LilvPluginClass* plugin_class;
	LilvNodes*             data_uris;  ///< rdfs::seeAlso
	LilvPort**             ports;
	uint32_t               num_ports;
	bool                   loaded;
	bool                   parse_errors;
	bool                   replaced;
};

struct LilvPluginClassImpl {
	LilvWorld* world;
	LilvNode*  uri;
	LilvNode*  parent_uri;
	LilvNode*  label;
};

struct LilvInstancePimpl {
	LilvWorld* world;
	LilvLib*   lib;
};

typedef struct {
	bool  dyn_manifest;
	bool  filter_language;
	char* lv2_path;
} LilvOptions;

struct LilvWorldImpl {
	SerdWorld*         world;
	SerdModel*         model;
	SerdReader*        reader;
	unsigned           n_read_files;
	LilvPluginClass*   lv2_plugin_class;
	LilvPluginClasses* plugin_classes;
	LilvSpec*          specs;
	LilvPlugins*       plugins;
	LilvPlugins*       zombies;
	LilvNodes*         loaded_files;
	ZixTree*           libs;
	struct {
		SerdNode* dc_replaces;
		SerdNode* dman_DynManifest;
		SerdNode* doap_name;
		SerdNode* lv2_Plugin;
		SerdNode* lv2_Specification;
		SerdNode* lv2_appliesTo;
		SerdNode* lv2_binary;
		SerdNode* lv2_default;
		SerdNode* lv2_designation;
		SerdNode* lv2_extensionData;
		SerdNode* lv2_index;
		SerdNode* lv2_latency;
		SerdNode* lv2_maximum;
		SerdNode* lv2_microVersion;
		SerdNode* lv2_minimum;
		SerdNode* lv2_minorVersion;
		SerdNode* lv2_name;
		SerdNode* lv2_optionalFeature;
		SerdNode* lv2_port;
		SerdNode* lv2_portProperty;
		SerdNode* lv2_reportsLatency;
		SerdNode* lv2_requiredFeature;
		SerdNode* lv2_symbol;
		SerdNode* lv2_project;
		SerdNode* lv2_prototype;
		SerdNode* owl_Ontology;
		SerdNode* pset_Preset;
		SerdNode* pset_value;
		SerdNode* rdf_a;
		SerdNode* rdf_value;
		SerdNode* rdfs_Class;
		SerdNode* rdfs_label;
		SerdNode* rdfs_seeAlso;
		SerdNode* rdfs_subClassOf;
		SerdNode* state_state;
		SerdNode* xsd_base64Binary;
		SerdNode* xsd_boolean;
		SerdNode* xsd_decimal;
		SerdNode* xsd_double;
		SerdNode* xsd_int;
		SerdNode* xsd_integer;
		SerdNode* null_uri;
	} uris;
	LilvOptions opt;
};

typedef enum {
	LILV_VALUE_URI,
	LILV_VALUE_STRING,
	LILV_VALUE_INT,
	LILV_VALUE_FLOAT,
	LILV_VALUE_BOOL,
	LILV_VALUE_BLANK,
	LILV_VALUE_BLOB
} LilvNodeType;

struct LilvScalePointImpl {
	LilvNode* value;
	LilvNode* label;
};

struct LilvUIImpl {
	LilvWorld* world;
	LilvNode*  uri;
	LilvNode*  bundle_uri;
	LilvNode*  binary_uri;
	LilvNodes* classes;
};

typedef struct LilvVersion {
	int minor;
	int micro;
} LilvVersion;

/*
 *
 * Functions
 *
 */

LilvPort* lilv_port_new(LilvWorld*      world,
                        const SerdNode* node,
                        uint32_t        index,
                        const char*     symbol);
void      lilv_port_free(const LilvPlugin* plugin, LilvPort* port);

LilvPlugin*
lilv_plugin_new(LilvWorld*      world,
                const LilvNode* uri,
                const LilvNode* bundle_uri);

void        lilv_plugin_clear(LilvPlugin* plugin, const LilvNode* bundle_uri);
void        lilv_plugin_load_if_necessary(const LilvPlugin* plugin);
void        lilv_plugin_free(LilvPlugin* plugin);
LilvNode*   lilv_plugin_get_unique(const LilvPlugin* plugin,
                                   const SerdNode*   subject,
                                   const SerdNode*   predicate);

void      lilv_collection_free(LilvCollection* collection);
unsigned  lilv_collection_size(const LilvCollection* collection);
LilvIter* lilv_collection_begin(const LilvCollection* collection);
void*     lilv_collection_get(const LilvCollection* collection,
                              const LilvIter*       i);

LilvPluginClass* lilv_plugin_class_new(LilvWorld*      world,
                                       const SerdNode* parent_node,
                                       const SerdNode* uri,
                                       const char*     label);

void lilv_plugin_class_free(LilvPluginClass* plugin_class);

LilvLib*
lilv_lib_open(LilvWorld*               world,
              const LilvNode*          uri,
              const char*              bundle_path,
              const LV2_Feature*const* features);

const LV2_Descriptor* lilv_lib_get_plugin(LilvLib* lib, uint32_t index);
void                  lilv_lib_close(LilvLib* lib);

LilvNodes*         lilv_nodes_new(void);
LilvPlugins*       lilv_plugins_new(void);
LilvScalePoints*   lilv_scale_points_new(void);
LilvPluginClasses* lilv_plugin_classes_new(void);
LilvUIs*           lilv_uis_new(void);

LilvNode* lilv_world_get_manifest_uri(LilvWorld*      world,
                                      const LilvNode* bundle_uri);

const char* lilv_world_blank_node_prefix(LilvWorld* world);

SerdStatus lilv_world_load_file(LilvWorld*      world,
                                SerdReader*     reader,
                                const LilvNode* uri);

SerdStatus
lilv_world_load_graph(LilvWorld*      world,
                      const SerdNode* graph,
                      const LilvNode* uri);

LilvUI* lilv_ui_new(LilvWorld* world,
                    LilvNode*  uri,
                    LilvNode*  type_uri,
                    LilvNode*  binary_uri);

void lilv_ui_free(LilvUI* ui);

int lilv_header_compare_by_uri(const void* a, const void* b, void* user_data);
int lilv_lib_compare(const void* a, const void* b, void* user_data);

int lilv_ptr_cmp(const void* a, const void* b, void* user_data);
int lilv_resource_node_cmp(const void* a, const void* b, void* user_data);

static inline int
lilv_version_cmp(const LilvVersion* a, const LilvVersion* b)
{
	if (a->minor == b->minor && a->micro == b->micro) {
		return 0;
	} else if ((a->minor < b->minor)
	           || (a->minor == b->minor && a->micro < b->micro)) {
		return -1;
	} else {
		return 1;
	}
}

struct LilvHeader*
lilv_collection_get_by_uri(const ZixTree* seq, const LilvNode* uri);

LilvScalePoint* lilv_scale_point_new(LilvNode* value, LilvNode* label);
void            lilv_scale_point_free(LilvScalePoint* point);

LilvNodes*
lilv_world_find_nodes_internal(LilvWorld*      world,
                               const SerdNode* subject,
                               const SerdNode* predicate,
                               const SerdNode* object);

SerdModel*
lilv_world_filter_model(LilvWorld*      world,
                        SerdModel*      model,
                        const SerdNode* subject,
                        const SerdNode* predicate,
                        const SerdNode* object,
                        const SerdNode* graph);

#define FOREACH_MATCH(statement_name_, range) \
	for (const SerdStatement* statement_name_ = NULL; \
	     !serd_range_empty(range) && (statement_name_ = serd_range_front(range)); \
	     serd_range_next(range))

struct PatternIterator {
	const SerdStatement* statement;
	SerdRange*           range;
};

#define FOREACH_PAT(name_, model_, s_, p_, o_, g_) \
	for (SerdRange* r_ = serd_model_range(model_, s_, p_, o_, g_); \
	     !serd_range_empty(r_) || (serd_range_free(r_), false); \
	     serd_range_next(r_)) \
		for (const SerdStatement* name_ = serd_range_front(r_); name_; name_ = NULL)

LilvNodes*
lilv_nodes_from_range(LilvWorld* world, SerdRange* range, SerdField field);

char*  lilv_strjoin(const char* first, ...);
char*  lilv_strdup(const char* str);
char*  lilv_get_lang(void);
char*  lilv_expand(const char* path);
char*  lilv_dirname(const char* path);
int    lilv_copy_file(const char* src, const char* dst);
bool   lilv_path_exists(const char* path, const void* ignored);
char*  lilv_path_absolute(const char* path);
bool   lilv_path_is_absolute(const char* path);
char*  lilv_get_latest_copy(const char* path, const char* copy_path);
char*  lilv_path_relative_to(const char* path, const char* base);
bool   lilv_path_is_child(const char* path, const char* dir);
int    lilv_flock(FILE* file, bool lock);
char*  lilv_realpath(const char* path);
int    lilv_symlink(const char* oldpath, const char* newpath);
int    lilv_mkdir_p(const char* dir_path);
char*  lilv_path_join(const char* a, const char* b);
bool   lilv_file_equals(const char* a_path, const char* b_path);

char*
lilv_find_free_path(const char* in_path,
                    bool (*exists)(const char*, const void*),
                    const void* user_data);

void
lilv_dir_for_each(const char* path,
                  void*       data,
                  void (*f)(const char* path, const char* name, void* data));

typedef void (*LilvVoidFunc)(void);

/** dlsym wrapper to return a function pointer (without annoying warning) */
static inline LilvVoidFunc
lilv_dlfunc(void* handle, const char* symbol)
{
#ifdef _WIN32
	 return (LilvVoidFunc)GetProcAddress((HMODULE)handle, symbol);
#else
	typedef LilvVoidFunc (*VoidFuncGetter)(void*, const char*);
	VoidFuncGetter dlfunc = (VoidFuncGetter)dlsym;
	return dlfunc(handle, symbol);
#endif
}

#ifdef LILV_DYN_MANIFEST
static const LV2_Feature* const dman_features = { NULL };
#endif

#define LILV_ERROR(str)       fprintf(stderr, "%s(): error: " str, \
                                      __func__)
#define LILV_ERRORF(fmt, ...) fprintf(stderr, "%s(): error: " fmt, \
                                      __func__, __VA_ARGS__)
#define LILV_WARN(str)        fprintf(stderr, "%s(): warning: " str, \
                                      __func__)
#define LILV_WARNF(fmt, ...)  fprintf(stderr, "%s(): warning: " fmt, \
                                      __func__, __VA_ARGS__)
#define LILV_NOTE(str)        fprintf(stderr, "%s(): note: " str, \
                                      __func__)
#define LILV_NOTEF(fmt, ...)  fprintf(stderr, "%s(): note: " fmt, \
                                      __func__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LILV_INTERNAL_H */
