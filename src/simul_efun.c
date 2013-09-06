#include "std.h"
#include "lpc_incl.h"
#include "lex.h"
#include "simul_efun.h"
#include "otable.h"

/*
 * This file rewritten by Beek because it was inefficient and slow.  We
 * now keep track of two mappings:
 *     name -> index       and     index -> function
 * 
 * index->function is used at runtime since it's very fast.  name->index
 * is used at compile time.  It's sorted so we can search it in O(log n)
 * as opposed to a linear search on the function table.  Note that we
 * can't sort the function table b/c then indices wouldn't be preserved
 * across updates.
 *
 * note, the name list holds names for past and present simul_efuns and
 * is now sorted for finding entries faster etc.  The identifier hash
 * table is used at compile time.
 */

typedef struct {
  char *name;
  short index;
} simul_entry;

simul_entry *simul_names = 0;
simul_info_t *simuls = 0;
static int num_simul_efun = 0;
object_t *simul_efun_ob;

static void find_or_add_simul_efun PROT((program_t *, int, int));
static void remove_simuls PROT((void));

#ifdef DEBUGMALLOC_EXTENSIONS
void mark_simuls() {
    int i;

    for (i = 0; i < num_simul_efun; i++) 
	EXTRA_REF(BLOCK(simul_names[i].name))++;
}
#endif

/*
 * If there is a simul_efun file, then take care of it and extract all
 * information we need.
 */
void init_simul_efun P1(char *, file)
{
    char buf[512];
#ifdef LPC_TO_C
    lpc_object_t *compiled_version;
#endif
    object_t *new_ob;
    
    if (!file || !file[0]) {
	fprintf(stderr, "No simul_efun\n");
	return;
    }
    if (!strip_name(file, buf, sizeof buf))
	error("Ilegal simul_efun file name '%s'\n", file);
    
#ifdef LPC_TO_C
    compiled_version = (lpc_object_t *)lookup_object_hash(buf);
#endif

    if (file[strlen(file) - 2] != '.')
	strcat(buf, ".c");

    new_ob = load_object(buf, compiled_version);
    if (new_ob == 0) {
	fprintf(stderr, "The simul_efun file %s was not loaded.\n", buf);
	exit(-1);
    }
    set_simul_efun(new_ob);
}

static void remove_simuls() {
    int i;
    ident_hash_elem_t *ihe;
    /* inactivate all old simul_efuns */
    for (i=0; i<num_simul_efun; i++) {
	simuls[i].index = 0;
	simuls[i].func = 0;
    }
    for (i=0; i<num_simul_efun; i++) {
	if ((ihe = lookup_ident(simul_names[i].name))) {
	    if (ihe->dn.simul_num != -1)
		ihe->sem_value--;
	    ihe->dn.simul_num = -1;
	    ihe->token &= ~IHE_SIMUL;
	}
    }    
}

static
void get_simul_efuns P1(program_t *, prog)
{
    int i;
    int num_new = prog->num_functions_total;

    if (num_simul_efun) {
	remove_simuls();
	if (!num_new) {
	    FREE(simul_names);
	    FREE(simuls);
	} else {
	    /* will be resized later */
	    simul_names = RESIZE(simul_names, num_simul_efun + num_new,
				 simul_entry, TAG_SIMULS, "get_simul_efuns");
	    simuls = RESIZE(simuls, num_simul_efun + num_new,
			    simul_info_t, TAG_SIMULS, "get_simul_efuns: 2");
	}
    } else {
	if (num_new) {
	    simul_names = CALLOCATE(num_new, simul_entry, TAG_SIMULS, "get_simul_efuns");
	    simuls = CALLOCATE(num_new, simul_info_t, TAG_SIMULS, "get_simul_efuns: 2");
	}
    }
    for (i=0; i < num_new; i++) {
	program_t *nprog;
	int index;
	runtime_function_u *func_entry;
	
	if (prog->function_flags[i] & (NAME_NO_CODE | NAME_STATIC | NAME_PRIVATE))
	    continue;
	nprog = prog;
	index = i;
	func_entry = FIND_FUNC_ENTRY(nprog, index);
	
	while (nprog->function_flags[index] & NAME_INHERITED) {
	    nprog = nprog->inherit[func_entry->inh.offset].prog;
	    index = func_entry->inh.function_index_offset;
	    func_entry = FIND_FUNC_ENTRY(nprog, index);
	}
	
	find_or_add_simul_efun(nprog, func_entry->def.f_index, i);
    }
    
    if (num_simul_efun) {
	/* shrink to fit */
	simul_names = RESIZE(simul_names, num_simul_efun, simul_entry,
			     TAG_SIMULS, "get_simul_efuns");
	simuls = RESIZE(simuls, num_simul_efun, simul_info_t,
			TAG_SIMULS, "get_simul_efuns");
    }
}

#define compare_addrs(x,y) (x < y ? -1 : (x > y ? 1 : 0))

/*
 * Test if 'name' is a simul_efun. The string pointer MUST be a pointer to
 * a shared string.
 */
int find_simul_efun P1(char *, name)
{
    int first = 0;
    int last = num_simul_efun - 1;
    int i,j;
    
    while (first <= last) {
	j = (first + last)/2;
	i = compare_addrs(name, simul_names[j].name);
	if (i == -1) 
	    {
		last = j-1;
	    }
	else if (i == 1)
	    {
		first = j+1;
	    }
	else
	    return simul_names[j].index;
    }
    return -1;
}

/*
 * Define a new simul_efun
 */
static void
find_or_add_simul_efun P3(program_t *, prog, int, index, int, runtime_index) {
    ident_hash_elem_t *ihe;
    int first = 0;
    int last = num_simul_efun - 1;
    int i,j;
    compiler_function_t *funp = &prog->function_table[index];
    
    while (first <= last) {
	j = (first + last)/2;
	i = compare_addrs(funp->name, simul_names[j].name);
	if (i == -1) 
	    {
		last = j-1;
	    }
	else if (i == 1)
	    {
		first = j+1;
	    }
	else
	    {
		ihe = find_or_add_perm_ident(simul_names[j].name);
		ihe->token |= IHE_SIMUL;
		ihe->sem_value++;
		ihe->dn.simul_num = simul_names[j].index;
		simuls[simul_names[j].index].index = runtime_index;
		simuls[simul_names[j].index].func = funp;
		return;
	    }
    }
    for (i=num_simul_efun - 1; i > last; i--)
	simul_names[i+1] = simul_names[i];
    simuls[num_simul_efun].index = runtime_index;
    simuls[num_simul_efun].func = funp;
    simul_names[first].name = funp->name;
    simul_names[first].index = num_simul_efun;
    ihe = find_or_add_perm_ident(funp->name);
    ihe->token |= IHE_SIMUL;
    ihe->sem_value++;
    ihe->dn.simul_num = num_simul_efun++;
    ref_string(funp->name);
}

void
set_simul_efun P1(object_t *, ob) {
    get_simul_efuns(ob->prog);
    
    simul_efun_ob = ob;
    add_ref(simul_efun_ob, "set_simul_efun");
}
