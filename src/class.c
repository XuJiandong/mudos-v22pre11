#include "std.h"
#include "lpc_incl.h"

void dealloc_class P1(array_t *, p) {
    int i;

    for (i = p->size; i--;)
	free_svalue(&p->item[i], "dealloc_class");
    FREE((char *) p);
}

void free_class P1(array_t *, p)
{
    if (--(p->ref) > 0)
	return;

    dealloc_class(p);
}

array_t *allocate_class P2(class_def_t *, cld, int, has_values) {
    array_t *p;
    int n = cld->size;

    p = (array_t *)DXALLOC(sizeof(array_t) + sizeof(svalue_t) * (n - 1), TAG_CLASS, "allocate_class");
    p->ref = 1;
    p->size = n;
    if (has_values) {
	while (n--)
	    p->item[n] = *sp--;
    } else {
	while (n--)
	    p->item[n] = const0;
    }
    return p;
}

array_t *allocate_class_by_size P1(int, size) {
    array_t *p;

    p = (array_t *)DXALLOC(sizeof(array_t) + sizeof(svalue_t) * (size - 1), TAG_CLASS, "allocate_class");
    p->ref = 1;
    p->size = size;

    while (size--)
	p->item[size] = const0;

    return p;
}

