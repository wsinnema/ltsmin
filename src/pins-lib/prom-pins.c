#include <hre/config.h>

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dm/dm.h>
#include <hre/runtime.h>
#include <hre/unix.h>
#include <ltsmin-lib/ltsmin-standard.h>
#include <pins-lib/prom-pins.h>
#include <util-lib/chunk_support.h>
#include <util-lib/util.h>

static const char* ACCEPTING_STATE_LABEL_NAME       = "accept_";
static const char* NON_PROGRESS_STATE_LABEL_NAME    = "np_";
static const char* VALID_END_STATE_LABEL_NAME       = "end_";

/**
 * SpinS - LTSmin interface functions
 */

/* Next-state functions */
next_method_grey_t  prom_get_successor;
next_method_black_t prom_get_successor_all;
void        (*prom_get_initial_state)(int *to);

/* PINS dependency matrix info */
int         (*prom_get_state_size)();
int         (*prom_get_transition_groups)();
const int*  (*prom_get_transition_read_dependencies)(int t);
const int*  (*prom_get_transition_write_dependencies)(int t);

/* PINS state type/value info */
const char* (*prom_get_state_variable_name)(int var);
int         (*prom_get_state_variable_type)(int var);
const char* (*prom_get_type_name)(int type);
int         (*prom_get_type_count)();
const char* (*prom_get_type_value_name)(int type, int value);
int         (*prom_get_type_value_count)(int type);

/* PINS edge labels (could be optional) */
int         (*prom_get_edge_count)();
const char* (*prom_get_edge_name)(int type);
int         (*prom_get_edge_type)(int type);

/* PINS state labels (could be optional) */
int         (*prom_get_label_count) ();
int         (*prom_get_guard_count) (); // a subset of the labels
const int*  (*prom_get_label_matrix)(int g);

/* PINS POR matrices and label info (could be optional) */
const int*  (*prom_get_labels)      (int t);
const int** (*prom_get_all_labels)  ();
int         (*prom_get_label)       (void *, int g, int *src);
const char* (*prom_get_label_name)  (int g);
void        (*prom_get_labels_all)  (void *, int *src, int* labels);
const int*  (*prom_get_label_may_be_coenabled_matrix)(int g);
const int*  (*prom_get_label_nes_matrix)(int g); // could be optional for POR
const int*  (*prom_get_label_nds_matrix)(int g); // could be optional for POR
const int*  (*prom_get_label_visiblity_matrix)(int g); //

static void
prom_popt (poptContext con,
             enum poptCallbackReason reason,
             const struct poptOption *opt,
             const char *arg, void *data)
{
	(void)con;(void)opt;(void)arg;(void)data;
	switch(reason){
	case POPT_CALLBACK_REASON_PRE:
		break;
	case POPT_CALLBACK_REASON_POST:
        GBregisterPreLoader("pr",       PromCompileGreyboxModel);
        GBregisterPreLoader("promela",  PromCompileGreyboxModel);
        GBregisterPreLoader("prom",     PromCompileGreyboxModel);
        GBregisterPreLoader("prm",      PromCompileGreyboxModel);
        GBregisterPreLoader("pml",      PromCompileGreyboxModel);
		GBregisterPreLoader("spins",   PromLoadDynamicLib);

        GBregisterLoader("pr",          PromLoadGreyboxModel);
        GBregisterLoader("promela",     PromLoadGreyboxModel);
        GBregisterLoader("prom",        PromLoadGreyboxModel);
        GBregisterLoader("pml",         PromLoadGreyboxModel);
        GBregisterLoader("prm",         PromLoadGreyboxModel);
        GBregisterLoader("spins",      PromLoadGreyboxModel);
		Warning(info,"Precompiled spins module initialized");
		return;
	case POPT_CALLBACK_REASON_OPTION:
		break;
	}
	Abort("unexpected call to spins_popt");
}

struct poptOption prom_options[]= {
	{ NULL, 0 , POPT_ARG_CALLBACK|POPT_CBFLAG_POST|POPT_CBFLAG_SKIPOPTION , (void*)&prom_popt, 0 , NULL , NULL },
	POPT_TABLEEND
};

typedef struct grey_box_context {
    int todo;
} *gb_context_t;

static void* dlHandle = NULL;

/**
 * Compile a promela model to a .spins binary using spinsl
 * calls: PromLoadDynamicLib
 */
void
PromCompileGreyboxModel(model_t model, const char *filename)
{
    struct stat st;
    int ret;

    // check file exists
    if ((ret = stat (filename, &st)) != 0)
        Abort("File not found: %s", filename);

    // compile opaal model
    char command[4096];
    if (snprintf(command, sizeof command, "spins '%s'", filename) >= (ssize_t)sizeof command)
        Abort("Cannot compile `%s', paths too long", filename);

    if ((ret = system(command)) != 0)
        HREassert(ret >= 0, "Command failed with exit code %d: %s", ret, command);

    char *basename = gnu_basename ((char *)filename);
    // check existence of bin file
    char *bin_fname = RTmallocZero(strlen(basename) + strlen(".spins") + 1);
    strncpy(bin_fname, basename, strlen(basename));
    char *ext = bin_fname + strlen(basename);
    strncpy(ext, ".spins", strlen(".spins"));

    if ((ret = stat (bin_fname, &st)) != 0)
        HREassert(ret >= 0, "File not found: %s", bin_fname);

    PromLoadDynamicLib(model, bin_fname);
    RTfree (bin_fname);
}

/**
 * Load a .spins binary as a dynamic library
 */
void
PromLoadDynamicLib(model_t model, const char *filename)
{
    // Open spins file
    char abs_filename[PATH_MAX];
    char *ret_filename = realpath(filename, abs_filename);
    if (ret_filename != NULL) {
        dlHandle = dlopen (abs_filename, RTLD_LAZY);
        if (dlHandle == NULL) {
            Abort("%s, Library \"%s\" is not reachable", dlerror(), filename);
            return;
        }
    } else {
        Abort("%s, Library \"%s\" is not found", dlerror(), filename);
    }

    // load dynamic library functionality
    prom_get_initial_state = (void(*)(int*))
        RTdlsym( filename, dlHandle, "spins_get_initial_state" );
    prom_get_successor = (next_method_grey_t)
        RTdlsym( filename, dlHandle, "spins_get_successor" );
    prom_get_successor_all = (next_method_black_t)
        RTdlsym( filename, dlHandle, "spins_get_successor_all" );
    prom_get_state_size = (int(*)())
        RTdlsym( filename, dlHandle, "spins_get_state_size" );
    prom_get_transition_groups = (int(*)())
        RTdlsym( filename, dlHandle, "spins_get_transition_groups" );
    prom_get_transition_read_dependencies = (const int*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_transition_read_dependencies" );
    prom_get_transition_write_dependencies = (const int*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_transition_write_dependencies" );
    prom_get_state_variable_name = (const char*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_state_variable_name" );
    prom_get_state_variable_type = (int (*)(int))
        RTdlsym( filename, dlHandle, "spins_get_state_variable_type" );
    prom_get_type_name = (const char*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_type_name" );
    prom_get_type_count = (int(*)())
        RTdlsym( filename, dlHandle, "spins_get_type_count" );
    prom_get_type_value_name = (const char*(*)(int,int))
        RTdlsym( filename, dlHandle, "spins_get_type_value_name" );
    prom_get_type_value_count = (int(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_type_value_count" );
    prom_get_edge_count = (int(*)())
        RTdlsym( filename, dlHandle, "spins_get_edge_count" );
    prom_get_edge_name = (const char*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_edge_name" );
    prom_get_edge_type = (int(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_edge_type" );
    prom_get_label_count = (int(*)())
        RTdlsym( filename, dlHandle, "spins_get_label_count" );
    prom_get_guard_count = (int(*)())
        RTdlsym( filename, dlHandle, "spins_get_guard_count" );
    prom_get_label_matrix = (const int*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_label_matrix" );
    prom_get_labels = (const int*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_labels" );
    prom_get_all_labels = (const int**(*)())
        RTdlsym( filename, dlHandle, "spins_get_all_labels" );
    prom_get_label = (int(*)(void*,int,int*))
        RTdlsym( filename, dlHandle, "spins_get_label" );
    prom_get_label_name = (const char*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_label_name" );
    prom_get_labels_all = (void(*)(void*,int*,int*))
        RTdlsym( filename, dlHandle, "spins_get_labels_all" );
    prom_get_label_may_be_coenabled_matrix = (const int*(*)(int))
        RTdlsym( filename, dlHandle, "spins_get_label_may_be_coenabled_matrix" );
    // optional POR functionality (NES/NDS):
    prom_get_label_nes_matrix = (const int*(*)(int))
        RTtrydlsym( dlHandle, "spins_get_label_nes_matrix" );
    prom_get_label_nds_matrix = (const int*(*)(int))
        RTtrydlsym( dlHandle, "spins_get_label_nds_matrix" );
    prom_get_label_visiblity_matrix = (const int*(*)(int))
        RTtrydlsym( dlHandle, "spins_get_label_visiblity_matrix" );

    (void)model;
}

void
sl_group (model_t model, sl_group_enum_t group, int*src, int *label)
{
    prom_get_labels_all (model, src, label);
    (void) group; // Both groups overlap, and start at index 0!
}

/**
 * Load .spins information into PINS (and ltstype)
 */
void
PromLoadGreyboxModel(model_t model, const char *filename)
{
    lts_type_t ltstype;
    matrix_t *dm_info = RTmalloc (sizeof *dm_info);
    matrix_t *dm_read_info = RTmalloc(sizeof(matrix_t));
    matrix_t *dm_write_info = RTmalloc(sizeof(matrix_t));
    matrix_t *dm_visibility_info = RTmalloc(sizeof(matrix_t));
    matrix_t *sl_info = RTmalloc (sizeof *sl_info);

    // assume sequential use (preLoader may not have been called):
    if (NULL == dlHandle) {
        char *extension = strrchr (filename, '.');
        HREassert (extension != NULL, "No filename extension in %s", filename);
        if (0==strcmp (extension, ".spins")) {
            PromLoadDynamicLib (model, filename);
        } else {
            PromCompileGreyboxModel(model, filename);
        }
    }

    gb_context_t ctx=(gb_context_t)RTmalloc(sizeof(struct grey_box_context));
    GBsetContext(model,ctx);

    // get ltstypes
    ltstype=lts_type_create();
    int state_length = prom_get_state_size();

    // adding types
    int ntypes = prom_get_type_count();
    for (int i = 0; i < ntypes; i++) {
        const char* type_name = prom_get_type_name(i);
        HREassert (type_name != NULL, "invalid type name");
        if (lts_type_add_type(ltstype, type_name, NULL) != i) {
            Abort("wrong type number");
        }
        int type_value_count = prom_get_type_value_count(i);
        Debug("Promela type %s (%d) has %d values.", type_name, i, type_value_count);
        if (0 == type_value_count) {
            lts_type_set_format (ltstype, i, LTStypeDirect);
        } else {
             lts_type_set_format (ltstype, i, LTStypeEnum);
        }
    }

    int bool_is_new, bool_type = lts_type_add_type (ltstype, LTSMIN_TYPE_BOOL, &bool_is_new);

    lts_type_set_state_length(ltstype, state_length);

    // set state name & type
    for (int i=0; i < state_length; ++i) {
        const char* name = prom_get_state_variable_name(i);
        const int   type = prom_get_state_variable_type(i);
        lts_type_set_state_name(ltstype,i,name);
        lts_type_set_state_typeno(ltstype,i,type);
    }

    // edge label types
    lts_type_set_edge_label_count (ltstype, prom_get_edge_count());
    for (int i = 0; i < prom_get_edge_count(); i++) {
        lts_type_set_edge_label_name(ltstype, i, prom_get_edge_name(i));
        int typeno = prom_get_edge_type(i);
        const char* type_name = prom_get_type_name(typeno);
        lts_type_set_edge_label_type(ltstype, i, type_name);
        lts_type_set_edge_label_typeno(ltstype, i, typeno);
    }

    GBsetLTStype(model, ltstype); // must set ltstype before setting initial state
                                  // creates tables for types!

    // get initial state
    int state[state_length];
    prom_get_initial_state(state);
    GBsetInitialState(model,state);

    // get next state
    GBsetNextStateAll  (model, prom_get_successor_all);
    GBsetNextStateLong (model, prom_get_successor);

    if (bool_is_new) {
        GBchunkPutAt(model, bool_type, chunk_str(LTSMIN_VALUE_BOOL_FALSE), 0);
        GBchunkPutAt(model, bool_type, chunk_str(LTSMIN_VALUE_BOOL_TRUE ), 1);
    }

    // setting values for types
    for(int i=0; i < ntypes; i++) {
        int type_value_count = prom_get_type_value_count(i);
        for(int j=0; j < type_value_count; ++j) {
            const char* type_value = prom_get_type_value_name(i, j);
            GBchunkPutAt(model, i, chunk_str((char*)type_value), j);
        }
    }

	// init state labels
	int sl_size = prom_get_label_count();
    int nguards = prom_get_guard_count();
	lts_type_set_state_label_count (ltstype, sl_size);

    for(int i = 0;i < sl_size; i++) {
        const char *name = prom_get_label_name (i);
        HREassert (i >= nguards || has_prefix(name, LTSMIN_LABEL_TYPE_GUARD_PREFIX),
                   "Label %d was expected to ba a guard instead of '%s'", i, name);
        lts_type_set_state_label_name (ltstype, i, name);
        lts_type_set_state_label_typeno (ltstype, i, bool_type);
    }

    lts_type_validate(ltstype); // done with ltstype

    // set the label group implementation
    sl_group_t* sl_group_all = RTmallocZero(sizeof(sl_group_t) + sl_size * sizeof(int));
    sl_group_all->count = sl_size;
    for(int i=0; i < sl_group_all->count; i++) sl_group_all->sl_idx[i] = i;
    GBsetStateLabelGroupInfo(model, GB_SL_ALL, sl_group_all);
    if (nguards > 0) {
        sl_group_t* sl_group_guards = RTmallocZero(sizeof(sl_group_t) + nguards * sizeof(int));
        sl_group_guards->count = nguards;
        for(int i=0; i < sl_group_guards->count; i++) sl_group_guards->sl_idx[i] = i;
        GBsetStateLabelGroupInfo(model, GB_SL_GUARDS, sl_group_guards);
    }

    // get state labels
    GBsetStateLabelsGroup(model, sl_group);
    GBsetStateLabelLong(model, (get_label_method_t)prom_get_label);
    GBsetStateLabelsAll(model, (get_label_all_method_t)prom_get_labels_all);

    // check for properties (label order: guard,..,guard,accept,end,progress,etc)
    for(int i = nguards; i < sl_size; i++) {
        const char *name = prom_get_label_name (i);
        if (strcmp (ACCEPTING_STATE_LABEL_NAME, name) == 0) {
            GBsetAcceptingStateLabelIndex (model, i);
        }
        if(strcmp(NON_PROGRESS_STATE_LABEL_NAME, name) == 0) {
            GBsetProgressStateLabelIndex (model, i);
        }
        if(strcmp(VALID_END_STATE_LABEL_NAME, name) == 0) {
            GBsetValidEndStateLabelIndex (model, i);
        }
    }

    // initialize the state read/write dependency matrices
    int ngroups = prom_get_transition_groups();
    dm_create(dm_info, ngroups, state_length);
    dm_create(dm_read_info, ngroups, state_length);
    dm_create(dm_write_info, ngroups, state_length);
    for (int i=0; i < dm_nrows(dm_info); i++) {
        int* proj = (int*)prom_get_transition_read_dependencies(i);
        HREassert (proj != NULL, "No SpinS read dependencies");
        for(int j=0; j<state_length; j++) {
            if (proj[j]) dm_set(dm_info, i, j);
            if (proj[j]) dm_set(dm_read_info, i, j);
        }
        proj = (int*)prom_get_transition_write_dependencies(i);
        HREassert (proj != NULL, "No SpinS write dependencies");
        for(int j=0; j<state_length; j++) {
            if (proj[j]) dm_set(dm_info, i, j);
            if (proj[j]) dm_set(dm_write_info, i, j);
        }
    }
    GBsetDMInfo(model, dm_info);
    GBsetDMInfoRead(model, dm_read_info);
    GBsetDMInfoWrite(model, dm_write_info);

    dm_create(dm_visibility_info, sl_size, ngroups);
    for (int i=0; i < dm_nrows(dm_visibility_info); i++) {
        const int *visible = prom_get_label_visiblity_matrix(i);
        for(int j=0; j<dm_ncols(dm_visibility_info); j++) {
            if (visible[j]) {
                dm_set(dm_visibility_info, i, j);
            }
        }
    }
    GBsetStateLabelVisibilityInfo(model, dm_visibility_info);

    // initialize state label dependency matrix
    dm_create(sl_info, sl_size, state_length);
    for(int i = 0; i < sl_size; i++) {
        int *guards = (int*)prom_get_label_matrix(i);
        for(int j = 0; j<state_length; j++) {
            if (guards[j]) dm_set(sl_info, i, j);
        }
    }
    GBsetStateLabelInfo(model, sl_info);

    // set the guards per transition group
    GBsetGuardsInfo(model, (guard_t**) prom_get_all_labels());

    // set guard may be co-enabled relation
    if (prom_get_label_may_be_coenabled_matrix) {
        matrix_t *gce_info = RTmalloc(sizeof(matrix_t));
        dm_create(gce_info, nguards, nguards);
        for (int i = 0; i < nguards; i++) {
            int *guardce = (int*)prom_get_label_may_be_coenabled_matrix(i);
            for(int j = 0; j < nguards; j++) {
                if (guardce[j]) dm_set(gce_info, i, j);
            }
        }
        GBsetGuardCoEnabledInfo(model, gce_info);
    }

    // set guard necessary enabling set info
    if (prom_get_label_nes_matrix) {
        matrix_t *gnes_info = RTmalloc(sizeof(matrix_t));
        dm_create(gnes_info, nguards, ngroups);
        for(int i = 0; i < nguards; i++) {
            int *guardnes = (int*)prom_get_label_nes_matrix(i);
            for(int j = 0; j < ngroups; j++) {
                if (guardnes[j]) dm_set(gnes_info, i, j);
            }
        }
        GBsetGuardNESInfo(model, gnes_info);
    }

    // set guard necessary disabling set info
    if (prom_get_label_nds_matrix) {
        matrix_t *gnds_info = RTmalloc(sizeof(matrix_t));
        dm_create(gnds_info, nguards, ngroups);
        for(int i = 0; i < nguards; i++) {
            int *guardnds = (int*)prom_get_label_nds_matrix(i);
            for(int j = 0; j < ngroups; j++) {
                if (guardnds[j]) dm_set(gnds_info, i, j);
            }
        }
        GBsetGuardNDSInfo(model, gnds_info);
    }
}
