#include <sys/types.h>
#include <sys/acl.h>

#include <Python.h>

#ifdef HAVE_LINUX
#include "os_linux.c"
#endif

staticforward PyTypeObject ACL_Type;
static PyObject* ACL_applyto(PyObject* obj, PyObject* args);
static PyObject* ACL_valid(PyObject* obj, PyObject* args);

#ifdef HAVE_LEVEL2
static PyObject* ACL_get_state(PyObject *obj, PyObject* args);
static PyObject* ACL_set_state(PyObject *obj, PyObject* args);

staticforward PyTypeObject Entry_Type;
staticforward PyTypeObject Permset_Type;
static PyObject* Permset_new(PyTypeObject* type, PyObject* args, PyObject *keywds);
#endif

typedef struct {
    PyObject_HEAD
    acl_t acl;
    int entry_id;
} ACL_Object;

#ifdef HAVE_LEVEL2

typedef struct {
    PyObject_HEAD
    PyObject *parent_acl; /* The parent acl, so it won't run out on us */
    acl_entry_t entry;
} Entry_Object;

typedef struct {
    PyObject_HEAD
    PyObject *parent_entry; /* The parent entry, so it won't run out on us */
    acl_permset_t permset;
} Permset_Object;

#endif

/* Creation of a new ACL instance */
static PyObject* ACL_new(PyTypeObject* type, PyObject* args, PyObject *keywds) {
    PyObject* newacl;

    newacl = type->tp_alloc(type, 0);

    if(newacl != NULL) {
        ((ACL_Object*)newacl)->acl = NULL;
        ((ACL_Object*)newacl)->entry_id = ACL_FIRST_ENTRY;
    }

    return newacl;
}

/* Initialization of a new ACL instance */
static int ACL_init(PyObject* obj, PyObject* args, PyObject *keywds) {
    ACL_Object* self = (ACL_Object*) obj;
    static char *kwlist[] = { "file", "fd", "text", "acl", "filedef", NULL };
    char *file = NULL;
    char *filedef = NULL;
    char *text = NULL;
    int fd = -1;
    ACL_Object* thesrc = NULL;

    if(!PyTuple_Check(args) || PyTuple_Size(args) != 0 ||
       (keywds != NULL && PyDict_Check(keywds) && PyDict_Size(keywds) > 1)) {
        PyErr_SetString(PyExc_ValueError, "a max of one keyword argument must be passed");
        return -1;
    }
    if(!PyArg_ParseTupleAndKeywords(args, keywds, "|sisO!s", kwlist,
                                     &file, &fd, &text, &ACL_Type, &thesrc, &filedef))
        return -1;

    /* Free the old acl_t without checking for error, we don't
     * care right now */
    if(self->acl != NULL)
        acl_free(self->acl);

    if(file != NULL)
        self->acl = acl_get_file(file, ACL_TYPE_ACCESS);
    else if(text != NULL)
        self->acl = acl_from_text(text);
    else if(fd != -1)
        self->acl = acl_get_fd(fd);
    else if(thesrc != NULL)
        self->acl = acl_dup(thesrc->acl);
    else if(filedef != NULL)
        self->acl = acl_get_file(filedef, ACL_TYPE_DEFAULT);
    else
        self->acl = acl_init(0);

    if(self->acl == NULL) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    return 0;
}

/* Standard type functions */
static void ACL_dealloc(PyObject* obj) {
    ACL_Object *self = (ACL_Object*) obj;
    PyObject *err_type, *err_value, *err_traceback;
    int have_error = PyErr_Occurred() ? 1 : 0;

    if (have_error)
        PyErr_Fetch(&err_type, &err_value, &err_traceback);
    if(self->acl != NULL && acl_free(self->acl) != 0)
        PyErr_WriteUnraisable(obj);
    if (have_error)
        PyErr_Restore(err_type, err_value, err_traceback);
    PyObject_DEL(self);
}

/* Converts the acl to a text format */
static PyObject* ACL_str(PyObject *obj) {
    char *text;
    ACL_Object *self = (ACL_Object*) obj;
    PyObject *ret;

    text = acl_to_text(self->acl, NULL);
    if(text == NULL) {
        return PyErr_SetFromErrno(PyExc_IOError);
    }
    ret = PyString_FromString(text);
    if(acl_free(text) != 0) {
        Py_DECREF(ret);
        return PyErr_SetFromErrno(PyExc_IOError);
    }
    return ret;
}

/* Custom methods */
static char __applyto_doc__[] = \
"Apply the ACL to a file or filehandle.\n" \
"\n" \
"Parameters:\n" \
"  - either a filename or a file-like object or an integer; this\n" \
"    represents the filesystem object on which to act\n" \
"  - optional flag representing the type of ACL to set, either\n" \
"    ACL_TYPE_ACCESS (default) or ACL_TYPE_DEFAULT\n" \
;

/* Applyes the ACL to a file */
static PyObject* ACL_applyto(PyObject* obj, PyObject* args) {
    ACL_Object *self = (ACL_Object*) obj;
    PyObject *myarg;
    acl_type_t type = ACL_TYPE_ACCESS;
    int nret;
    int fd;

    if (!PyArg_ParseTuple(args, "O|i", &myarg, &type))
        return NULL;

    if(PyString_Check(myarg)) {
        char *filename = PyString_AS_STRING(myarg);
        nret = acl_set_file(filename, type, self->acl);
    } else if((fd = PyObject_AsFileDescriptor(myarg)) != -1) {
        nret = acl_set_fd(fd, self->acl);
    } else {
        PyErr_SetString(PyExc_TypeError, "argument 1 must be string, int, or file-like object");
        return 0;
    }
    if(nret == -1) {
        return PyErr_SetFromErrno(PyExc_IOError);
    }

    /* Return the result */
    Py_INCREF(Py_None);
    return Py_None;
}

static char __valid_doc__[] = \
"Test the ACL for validity.\n" \
"\n" \
"This method tests the ACL to see if it is a valid ACL\n" \
"in terms of the filesystem. More precisely, it checks:\n" \
"A valid ACL contains exactly one entry with each of the ACL_USER_OBJ,\n" \
"ACL_GROUP_OBJ, and ACL_OTHER tag types. Entries with ACL_USER and\n" \
"ACL_GROUP tag types may appear zero or more times in an ACL. An ACL that\n" \
"contains entries of ACL_USER or ACL_GROUP tag types must contain exactly\n" \
"one entry of the ACL_MASK tag type. If an ACL contains no entries of\n" \
"ACL_USER or ACL_GROUP tag types, the ACL_MASK entry is optional.\n" \
"\n" \
"All user ID qualifiers must be unique among all entries of ACL_USER tag\n" \
"type, and all group IDs must be unique among all entries of ACL_GROUP tag\n" \
"type.\n" \
"\n" \
"The method will return 1 for a valid ACL and 0 for an invalid one.\n" \
"This has been chosen because the specification for acl_valid in POSIX.1e\n" \
"documents only one possible value for errno in case of an invalid ACL, \n" \
"so we can't differentiate between classes of errors. Other suggestions \n" \
"are welcome.\n" \
;

/* Checks the ACL for validity */
static PyObject* ACL_valid(PyObject* obj, PyObject* args) {
    ACL_Object *self = (ACL_Object*) obj;

    if(acl_valid(self->acl) == -1) {
        Py_INCREF(Py_False);
        return Py_False;
    } else {
        Py_INCREF(Py_True);
        return Py_True;
    }
}

#ifdef HAVE_LEVEL2

static PyObject* ACL_get_state(PyObject *obj, PyObject* args) {
    ACL_Object *self = (ACL_Object*) obj;
    PyObject *ret;
    ssize_t size, nsize;
    char *buf;

    size = acl_size(self->acl);
    if(size == -1)
        return PyErr_SetFromErrno(PyExc_IOError);

    if((ret = PyString_FromStringAndSize(NULL, size)) == NULL)
        return NULL;
    buf = PyString_AsString(ret);
    
    if((nsize = acl_copy_ext(buf, self->acl, size)) == -1) {
        Py_DECREF(ret);
        return PyErr_SetFromErrno(PyExc_IOError);
    }
    
    return ret;
}

static PyObject* ACL_set_state(PyObject *obj, PyObject* args) {
    ACL_Object *self = (ACL_Object*) obj;
    const void *buf;
    int bufsize;
    acl_t ptr;

    /* Parse the argument */
    if (!PyArg_ParseTuple(args, "s#", &buf, &bufsize))
        return NULL;

    /* Try to import the external representation */
    if((ptr = acl_copy_int(buf)) == NULL)
        return PyErr_SetFromErrno(PyExc_IOError);
        
    /* Free the old acl. Should we ignore errors here? */
    if(self->acl != NULL) {
        if(acl_free(self->acl) == -1)
            return PyErr_SetFromErrno(PyExc_IOError);
    }

    self->acl = ptr;

    /* Return the result */
    Py_INCREF(Py_None);
    return Py_None;
}

/* tp_iter for the ACL type; since it can be iterated only
 * destructively, the type is its iterator
 */
static PyObject* ACL_iter(PyObject *obj) {
    ACL_Object *self = (ACL_Object*)obj;
    self->entry_id = ACL_FIRST_ENTRY;
    Py_INCREF(obj);
    return obj;
}

/* the tp_iternext function for the ACL type */
static PyObject* ACL_iternext(PyObject *obj) {
    ACL_Object *self = (ACL_Object*)obj;
    acl_entry_t the_entry_t;
    Entry_Object *the_entry_obj;
    int nerr;
    
    nerr = acl_get_entry(self->acl, self->entry_id, &the_entry_t);
    self->entry_id = ACL_NEXT_ENTRY;
    if(nerr == -1)
        return PyErr_SetFromErrno(PyExc_IOError);
    else if(nerr == 0) {
        /* Docs says this is not needed */
        /*PyErr_SetObject(PyExc_StopIteration, Py_None);*/
        return NULL;
    }

    the_entry_obj = (Entry_Object*) PyType_GenericNew(&Entry_Type, NULL, NULL);
    if(the_entry_obj == NULL)
        return NULL;
    
    the_entry_obj->entry = the_entry_t;

    the_entry_obj->parent_acl = obj;
    Py_INCREF(obj); /* For the reference we have in entry->parent */

    return (PyObject*)the_entry_obj;
}

static char __ACL_delentry_doc__[] = \
"Deletes an entry from the ACL.\n" \
"\n" \
"Note: Only with level 2\n" \
"Parameters:\n" \
" - the Entry object which should be deleted; note that after\n" \
"   this function is called, that object is unusable any longer\n" \
"   and should be deleted\n" \
;

/* Deletes an entry from the ACL */
static PyObject* ACL_delentry(PyObject *obj, PyObject *args) {
    ACL_Object *self = (ACL_Object*)obj;
    Entry_Object *e;

    if (!PyArg_ParseTuple(args, "O!", &Entry_Type, &e))
        return NULL;

    if(acl_delete_entry(self->acl, e->entry) == -1)
        return PyErr_SetFromErrno(PyExc_IOError);

    /* Return the result */
    Py_INCREF(Py_None);
    return Py_None;
}

/* Creation of a new Entry instance */
static PyObject* Entry_new(PyTypeObject* type, PyObject* args, PyObject *keywds) {
    PyObject* newentry;

    newentry = PyType_GenericNew(type, args, keywds);

    if(newentry != NULL) {
        ((Entry_Object*)newentry)->entry = NULL;
        ((Entry_Object*)newentry)->parent_acl = NULL;
    }

    return newentry;
}

/* Initialization of a new Entry instance */
static int Entry_init(PyObject* obj, PyObject* args, PyObject *keywds) {
    Entry_Object* self = (Entry_Object*) obj;
    ACL_Object* parent = NULL;

    if (!PyArg_ParseTuple(args, "O!", &ACL_Type, &parent))
        return -1;

    if(acl_create_entry(&parent->acl, &self->entry) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    self->parent_acl = (PyObject*)parent;
    Py_INCREF(parent);

    return 0;
}

/* Free the Entry instance */
static void Entry_dealloc(PyObject* obj) {
    Entry_Object *self = (Entry_Object*) obj;
    PyObject *err_type, *err_value, *err_traceback;
    int have_error = PyErr_Occurred() ? 1 : 0;

    if (have_error)
        PyErr_Fetch(&err_type, &err_value, &err_traceback);
    if(self->parent_acl != NULL) {
        Py_DECREF(self->parent_acl);
        self->parent_acl = NULL;
    }
    if (have_error)
        PyErr_Restore(err_type, err_value, err_traceback);
    PyObject_DEL(self);
}

/* Converts the entry to a text format */
static PyObject* Entry_str(PyObject *obj) {
    acl_tag_t tag;
    uid_t qualifier;
    void *p;
    PyObject *ret;
    PyObject *format, *list;
    Entry_Object *self = (Entry_Object*) obj;

    if(acl_get_tag_type(self->entry, &tag) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
    }
    if(tag == ACL_USER || tag == ACL_GROUP) {
        if((p = acl_get_qualifier(self->entry)) == NULL) {
            PyErr_SetFromErrno(PyExc_IOError);
            return NULL;
        }
        qualifier = *(uid_t*)p;
        acl_free(p);
    } else {
        qualifier = 0;
    }
    
    format = PyString_FromString("ACL entry for %s");
    if(format == NULL)
        return NULL;
    list = PyTuple_New(1);
    if(tag == ACL_UNDEFINED_TAG) {
        PyTuple_SetItem(list, 0, PyString_FromString("undefined type"));
    } else if(tag == ACL_USER_OBJ) {
        PyTuple_SetItem(list, 0, PyString_FromString("the owner"));
    } else if(tag == ACL_GROUP_OBJ) {
        PyTuple_SetItem(list, 0, PyString_FromString("the group"));
    } else if(tag == ACL_OTHER) {
        PyTuple_SetItem(list, 0, PyString_FromString("the others"));
    } else if(tag == ACL_USER) {
        PyTuple_SetItem(list, 0, PyString_FromFormat("user with uid %d", qualifier));
    } else if(tag == ACL_GROUP) {
        PyTuple_SetItem(list, 0, PyString_FromFormat("group with gid %d", qualifier));
    } else if(tag == ACL_MASK) {
        PyTuple_SetItem(list, 0, PyString_FromString("the mask"));
    } else {
        PyTuple_SetItem(list, 0, PyString_FromString("UNKNOWN_TAG_TYPE!"));
    }
    ret = PyString_Format(format, list);
    Py_DECREF(format);
    Py_DECREF(list);
    return ret;
}

/* Sets the tag type of the entry */
static int Entry_set_tag_type(PyObject* obj, PyObject* value, void* arg) {
    Entry_Object *self = (Entry_Object*) obj;

    if(value == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "tag type deletion is not supported");
        return -1;
    }

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "tag type must be integer");
        return -1;
    }
    if(acl_set_tag_type(self->entry, (acl_tag_t)PyInt_AsLong(value)) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    return 0;
}

/* Returns the tag type of the entry */
static PyObject* Entry_get_tag_type(PyObject *obj, void* arg) {
    Entry_Object *self = (Entry_Object*) obj;
    acl_tag_t value;

    if (self->entry == NULL) {
        PyErr_SetString(PyExc_AttributeError, "entry attribute");
        return NULL;
    }
    if(acl_get_tag_type(self->entry, &value) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
    }

    return PyInt_FromLong(value);
}

/* Sets the qualifier (either uid_t or gid_t) for the entry,
 * usable only if the tag type if ACL_USER or ACL_GROUP
 */
static int Entry_set_qualifier(PyObject* obj, PyObject* value, void* arg) {
    Entry_Object *self = (Entry_Object*) obj;
    int uidgid;

    if(value == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "qualifier deletion is not supported");
        return -1;
    }

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "tag type must be integer");
        return -1;
    }
    uidgid = PyInt_AsLong(value);
    if(acl_set_qualifier(self->entry, (void*)&uidgid) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    return 0;
}

/* Returns the qualifier of the entry */
static PyObject* Entry_get_qualifier(PyObject *obj, void* arg) {
    Entry_Object *self = (Entry_Object*) obj;
    void *p;
    int value;

    if (self->entry == NULL) {
        PyErr_SetString(PyExc_AttributeError, "entry attribute");
        return NULL;
    }
    if((p = acl_get_qualifier(self->entry)) == NULL) {
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
    }
    value = *(uid_t*)p;
    acl_free(p);
    
    return PyInt_FromLong(value);
}

/* Returns the parent ACL of the entry */
static PyObject* Entry_get_parent(PyObject *obj, void* arg) {
    Entry_Object *self = (Entry_Object*) obj;
    
    Py_INCREF(self->parent_acl);
    return self->parent_acl;
}

/* Returns the a new Permset representing the permset of the entry 
 * FIXME: Should return a new reference to the same object, which
 * should be created at init time!
*/
static PyObject* Entry_get_permset(PyObject *obj, void* arg) {
    Entry_Object *self = (Entry_Object*)obj;
    PyObject *p;
    Permset_Object *ps;

    p = Permset_new(&Permset_Type, NULL, NULL);
    if(p == NULL)
        return NULL;
    ps = (Permset_Object*)p;
    if(acl_get_permset(self->entry, &ps->permset) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
    }
    ps->parent_entry = obj;
    Py_INCREF(obj);

    return (PyObject*)p;
}

/* Sets the permset of the entry to the passed Permset */
static int Entry_set_permset(PyObject* obj, PyObject* value, void* arg) {
    Entry_Object *self = (Entry_Object*)obj;
    Permset_Object *p;

    if(!PyObject_IsInstance(value, (PyObject*)&Permset_Type)) {
        PyErr_SetString(PyExc_TypeError, "argument 1 must be posix1e.Permset");
        return -1;
    }
    p = (Permset_Object*)value;
    if(acl_set_permset(self->entry, p->permset) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }
    return 0;
}

static char __Entry_copy_doc__[] = \
"Copy an ACL entry.\n" \
"\n" \
"This method sets all the parameters to those of another\n" \
"entry, even one of another's ACL\n" \
"Parameters:\n" \
" - src, instance of type Entry\n" \
;

/* Sets all the entry parameters to another's entry */
static PyObject* Entry_copy(PyObject *obj, PyObject *args) {
    Entry_Object *self = (Entry_Object*)obj;
    Entry_Object *other;
    
    if(!PyArg_ParseTuple(args, "O!", &Entry_Type, &other))
        return NULL;

    if(acl_copy_entry(self->entry, other->entry) == -1)
        return PyErr_SetFromErrno(PyExc_IOError);

    Py_INCREF(Py_None);
    return Py_None;
}

/**** Permset type *****/

/* Creation of a new Permset instance */
static PyObject* Permset_new(PyTypeObject* type, PyObject* args, PyObject *keywds) {
    PyObject* newpermset;

    newpermset = PyType_GenericNew(type, args, keywds);

    if(newpermset != NULL) {
        ((Permset_Object*)newpermset)->permset = NULL;
        ((Permset_Object*)newpermset)->parent_entry = NULL;
    }

    return newpermset;
}

/* Initialization of a new Permset instance */
static int Permset_init(PyObject* obj, PyObject* args, PyObject *keywds) {
    Permset_Object* self = (Permset_Object*) obj;
    Entry_Object* parent = NULL;

    if (!PyArg_ParseTuple(args, "O!", &Entry_Type, &parent))
        return -1;

    if(acl_get_permset(parent->entry, &self->permset) == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    self->parent_entry = (PyObject*)parent;
    Py_INCREF(parent);

    return 0;
}

/* Free the Permset instance */
static void Permset_dealloc(PyObject* obj) {
    Permset_Object *self = (Permset_Object*) obj;
    PyObject *err_type, *err_value, *err_traceback;
    int have_error = PyErr_Occurred() ? 1 : 0;

    if (have_error)
        PyErr_Fetch(&err_type, &err_value, &err_traceback);
    if(self->parent_entry != NULL) {
        Py_DECREF(self->parent_entry);
        self->parent_entry = NULL;
    }
    if (have_error)
        PyErr_Restore(err_type, err_value, err_traceback);
    PyObject_DEL(self);
}

/* Permset string representation */
static PyObject* Permset_str(PyObject *obj) {
    Permset_Object *self = (Permset_Object*) obj;
    char pstr[3];

    pstr[0] = get_perm(self->permset, ACL_READ) ? 'r' : '-';
    pstr[1] = get_perm(self->permset, ACL_WRITE) ? 'w' : '-';
    pstr[2] = get_perm(self->permset, ACL_EXECUTE) ? 'x' : '-';
    return PyString_FromStringAndSize(pstr, 3);
}

static char __Permset_clear_doc__[] = \
"Clear all permissions from the permission set.\n" \
;

/* Clears all permissions from the permset */
static PyObject* Permset_clear(PyObject* obj, PyObject* args) {
    Permset_Object *self = (Permset_Object*) obj;

    if(acl_clear_perms(self->permset) == -1)
        return PyErr_SetFromErrno(PyExc_IOError);

    /* Return the result */
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* Permset_get_right(PyObject *obj, void* arg) {
    Permset_Object *self = (Permset_Object*) obj;

    if(get_perm(self->permset, (int)arg)) {
        Py_INCREF(Py_True);
        return Py_True;
    } else {
        Py_INCREF(Py_False);
        return Py_False;
    }
}

static int Permset_set_right(PyObject* obj, PyObject* value, void* arg) {
    Permset_Object *self = (Permset_Object*) obj;
    int on;
    int nerr;

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_ValueError, "a maximum of one argument must be passed");
        return -1;
    }        
    on = PyInt_AsLong(value);
    if(on)
        nerr = acl_add_perm(self->permset, (int)arg);
    else
        nerr = acl_delete_perm(self->permset, (int)arg);
    if(nerr == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }
    return 0;
}

#endif

static char __ACL_Type_doc__[] = \
"Type which represents a POSIX ACL\n" \
"\n" \
"Parameters:\n" \
"  Only one keword parameter should be provided:\n"
"  - file=\"...\", meaning create ACL representing\n"
"    the access ACL of that file\n" \
"  - filedef=\"...\", meaning create ACL representing\n"
"    the default ACL of that directory\n" \
"  - fd=<int>, meaning create ACL representing\n" \
"    the access ACL of that file descriptor\n" \
"  - text=\"...\", meaning create ACL from a \n" \
"    textual description\n" \
"  - acl=<ACL instance>, meaning create a copy\n" \
"    of an existing ACL instance\n" \
"If no parameters are passed, create an empty ACL; this\n" \
"makes sense only when your OS supports ACL modification\n" \
" (i.e. it implements full POSIX.1e support)\n" \
;

/* ACL type methods */
static PyMethodDef ACL_methods[] = {
    {"applyto", ACL_applyto, METH_VARARGS, __applyto_doc__},
    {"valid", ACL_valid, METH_NOARGS, __valid_doc__},
#ifdef HAVE_LEVEL2
    {"__getstate__", ACL_get_state, METH_NOARGS, "Dumps the ACL to an external format."},
    {"__setstate__", ACL_set_state, METH_VARARGS, "Loads the ACL from an external format."},
    {"delentry", ACL_delentry, METH_VARARGS, __ACL_delentry_doc__},
#endif
    {NULL, NULL, 0, NULL}
};


/* The definition of the ACL Type */
static PyTypeObject ACL_Type = {
    PyObject_HEAD_INIT(NULL)
    0,
    "posix1e.ACL",
    sizeof(ACL_Object),
    0,
    ACL_dealloc,        /* tp_dealloc */
    0,                  /* tp_print */
    0,                  /* tp_getattr */
    0,                  /* tp_setattr */
    0,                  /* tp_compare */
    0,                  /* tp_repr */
    0,                  /* tp_as_number */
    0,                  /* tp_as_sequence */
    0,                  /* tp_as_mapping */
    0,                  /* tp_hash */
    0,                  /* tp_call */
    ACL_str,            /* tp_str */
    0,                  /* tp_getattro */
    0,                  /* tp_setattro */
    0,                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    __ACL_Type_doc__,   /* tp_doc */
    0,                  /* tp_traverse */
    0,                  /* tp_clear */
    0,                  /* tp_richcompare */
    0,                  /* tp_weaklistoffset */
#ifdef HAVE_LEVEL2
    ACL_iter,
    ACL_iternext,
#else
    0,                  /* tp_iter */
    0,                  /* tp_iternext */
#endif
    ACL_methods,        /* tp_methods */
    0,                  /* tp_members */
    0,                  /* tp_getset */
    0,                  /* tp_base */
    0,                  /* tp_dict */
    0,                  /* tp_descr_get */
    0,                  /* tp_descr_set */
    0,                  /* tp_dictoffset */
    ACL_init,           /* tp_init */
    0,                  /* tp_alloc */
    ACL_new,            /* tp_new */
};

#ifdef HAVE_LEVEL2

/* Entry type methods */
static PyMethodDef Entry_methods[] = {
    {"copy", Entry_copy, METH_VARARGS, __Entry_copy_doc__},
    {NULL, NULL, 0, NULL}
};

static char __Entry_tagtype_doc__[] = \
"The tag type of the current entry\n" \
"\n" \
"This is one of:\n" \
" - ACL_UNDEFINED_TAG\n" \
" - ACL_USER_OBJ\n" \
" - ACL_USER\n" \
" - ACL_GROUP_OBJ\n" \
" - ACL_GROUP\n" \
" - ACL_MASK\n" \
" - ACL_OTHER\n" \
;

static char __Entry_qualifier_doc__[] = \
"The qualifier of the current entry\n" \
"\n" \
"If the tag type is ACL_USER, this should be a user id.\n" \
"If the tag type if ACL_GROUP, this should be a group id.\n" \
"Else, it doesn't matter.\n" \
;

static char __Entry_parent_doc__[] = \
"The parent ACL of this entry\n" \
;

static char __Entry_permset_doc__[] = \
"The permission set of this ACL entry\n" \
;

/* Entry getset */
static PyGetSetDef Entry_getsets[] = {
    {"tag_type", Entry_get_tag_type, Entry_set_tag_type, __Entry_tagtype_doc__},
    {"qualifier", Entry_get_qualifier, Entry_set_qualifier, __Entry_qualifier_doc__},
    {"parent", Entry_get_parent, NULL, __Entry_parent_doc__},
    {"permset", Entry_get_permset, Entry_set_permset, __Entry_permset_doc__},
    {NULL}
};

static char __Entry_Type_doc__[] = \
"Type which represents an entry in an ACL.\n" \
"\n" \
"The type exists only if the OS has full support for POSIX.1e\n" \
"Can be created either by:\n" \
"  e = posix1e.Entry(myACL) # this creates a new entry in the ACL\n" \
"or by:\n" \
"  for entry in myACL:\n" \
"      print entry\n" \
;
/* The definition of the Entry Type */
static PyTypeObject Entry_Type = {
    PyObject_HEAD_INIT(NULL)
    0,
    "posix1e.Entry",
    sizeof(Entry_Object),
    0,
    Entry_dealloc,      /* tp_dealloc */
    0,                  /* tp_print */
    0,                  /* tp_getattr */
    0,                  /* tp_setattr */
    0,                  /* tp_compare */
    0,                  /* tp_repr */
    0,                  /* tp_as_number */
    0,                  /* tp_as_sequence */
    0,                  /* tp_as_mapping */
    0,                  /* tp_hash */
    0,                  /* tp_call */
    Entry_str,          /* tp_str */
    0,                  /* tp_getattro */
    0,                  /* tp_setattro */
    0,                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    __Entry_Type_doc__, /* tp_doc */
    0,                  /* tp_traverse */
    0,                  /* tp_clear */
    0,                  /* tp_richcompare */
    0,                  /* tp_weaklistoffset */
    0,                  /* tp_iter */
    0,                  /* tp_iternext */
    Entry_methods,      /* tp_methods */
    0,                  /* tp_members */
    Entry_getsets,      /* tp_getset */
    0,                  /* tp_base */
    0,                  /* tp_dict */
    0,                  /* tp_descr_get */
    0,                  /* tp_descr_set */
    0,                  /* tp_dictoffset */
    Entry_init,         /* tp_init */
    0,                  /* tp_alloc */
    Entry_new,          /* tp_new */
};

/* Permset type methods */
static PyMethodDef Permset_methods[] = {
    {"clear", Permset_clear, METH_NOARGS, __Permset_clear_doc__, },
    {NULL, NULL, 0, NULL}
};

static char __Permset_execute_doc__[] = \
"Execute permsission\n" \
;

static char __Permset_read_doc__[] = \
"Read permsission\n" \
;

static char __Permset_write_doc__[] = \
"Write permsission\n" \
;

/* Permset getset */
static PyGetSetDef Permset_getsets[] = {
    {"execute", Permset_get_right, Permset_set_right, __Permset_execute_doc__, (void*)ACL_EXECUTE},
    {"read", Permset_get_right, Permset_set_right, __Permset_read_doc__, (void*)ACL_READ},
    {"write", Permset_get_right, Permset_set_right, __Permset_write_doc__, (void*)ACL_WRITE},
    {NULL}
};

static char __Permset_Type_doc__[] = \
"Type which represents the permission set in an ACL entry\n" \
"\n" \
"The type exists only if the OS has full support for POSIX.1e\n" \
"Can be created either by:\n" \
"  perms = myEntry.permset\n" \
"or by:\n" \
"  perms = posix1e.Permset(myEntry)\n" \
;

/* The definition of the Permset Type */
static PyTypeObject Permset_Type = {
    PyObject_HEAD_INIT(NULL)
    0,
    "posix1e.Permset",
    sizeof(Permset_Object),
    0,
    Permset_dealloc,    /* tp_dealloc */
    0,                  /* tp_print */
    0,                  /* tp_getattr */
    0,                  /* tp_setattr */
    0,                  /* tp_compare */
    0,                  /* tp_repr */
    0,                  /* tp_as_number */
    0,                  /* tp_as_sequence */
    0,                  /* tp_as_mapping */
    0,                  /* tp_hash */
    0,                  /* tp_call */
    Permset_str,        /* tp_str */
    0,                  /* tp_getattro */
    0,                  /* tp_setattro */
    0,                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    __Permset_Type_doc__,/* tp_doc */
    0,                  /* tp_traverse */
    0,                  /* tp_clear */
    0,                  /* tp_richcompare */
    0,                  /* tp_weaklistoffset */
    0,                  /* tp_iter */
    0,                  /* tp_iternext */
    Permset_methods,    /* tp_methods */
    0,                  /* tp_members */
    Permset_getsets,    /* tp_getset */
    0,                  /* tp_base */
    0,                  /* tp_dict */
    0,                  /* tp_descr_get */
    0,                  /* tp_descr_set */
    0,                  /* tp_dictoffset */
    Permset_init,       /* tp_init */
    0,                  /* tp_alloc */
    Permset_new,        /* tp_new */
};

#endif

/* Module methods */

static char __deletedef_doc__[] = \
"Delete the default ACL from a directory.\n" \
"\n" \
"This function deletes the default ACL associated with \n" \
"a directory (the ACL which will be ANDed with the mode\n" \
"parameter to the open, creat functions).\n" \
"Parameters:\n" \
"  - a string representing the directory whose default ACL\n" \
"    should be deleted\n" \
;

/* Deletes the default ACL from a directory */
static PyObject* aclmodule_delete_default(PyObject* obj, PyObject* args) {
    char *filename;

    /* Parse the arguments */
    if (!PyArg_ParseTuple(args, "s", &filename))
        return NULL;

    if(acl_delete_def_file(filename) == -1) {
        return PyErr_SetFromErrno(PyExc_IOError);
    }

    /* Return the result */
    Py_INCREF(Py_None);
    return Py_None;
}

/* The module methods */
static PyMethodDef aclmodule_methods[] = {
    {"delete_default", aclmodule_delete_default, METH_VARARGS, __deletedef_doc__},
    {NULL, NULL, 0, NULL}
};

static char __posix1e_doc__[] = \
"POSIX.1e ACLs manipulation\n" \
"\n" \
"This module provides support for manipulating POSIX.1e ACLS\n" \
"\n" \
"Depending on the operating system support for POSIX.1e, \n" \
"the ACL type will have more or less capabilities:\n" \
"  - level 1, only basic support, you can create\n" \
"    ACLs from files and text descriptions;\n" \
"    once created, the type is immutable\n" \
"  - level 2, complete support, you can alter\n"\
"    the ACL once it is created\n" \
"\n" \
"Also, in level 2, more types are available, corresponding\n" \
"to acl_entry_t (Entry type), acl_permset_t (Permset type).\n" \
"\n" \
"Example:\n" \
">>> import posix1e\n" \
">>> acl1 = posix1e.ACL(file=\"file.txt\") \n" \
">>> print acl1\n" \
"user::rw-\n" \
"group::rw-\n" \
"other::r--\n" \
"\n" \
">>> b = posix1e.ACL(text=\"u::rx,g::-,o::-\")\n" \
">>> print b\n" \
"user::r-x\n" \
"group::---\n" \
"other::---\n" \
"\n" \
">>> b.applyto(\"file.txt\")\n" \
">>> print posix1e.ACL(file=\"file.txt\")\n" \
"user::r-x\n" \
"group::---\n" \
"other::---\n" \
"\n" \
">>>\n" \
;

void initposix1e(void) {
    PyObject *m, *d;

    ACL_Type.ob_type = &PyType_Type;
    if(PyType_Ready(&ACL_Type) < 0)
        return;

#ifdef HAVE_LEVEL2
    Entry_Type.ob_type = &PyType_Type;
    if(PyType_Ready(&Entry_Type) < 0)
        return;

    Permset_Type.ob_type = &PyType_Type;
    if(PyType_Ready(&Permset_Type) < 0)
        return;
#endif

    m = Py_InitModule3("posix1e", aclmodule_methods, __posix1e_doc__);

    d = PyModule_GetDict(m);
    if (d == NULL)
        return;

    Py_INCREF(&ACL_Type);
    if (PyDict_SetItemString(d, "ACL",
                             (PyObject *) &ACL_Type) < 0)
        return;

    /* 23.3.6 acl_type_t values */
    PyModule_AddIntConstant(m, "ACL_TYPE_ACCESS", ACL_TYPE_ACCESS);
    PyModule_AddIntConstant(m, "ACL_TYPE_DEFAULT", ACL_TYPE_DEFAULT);


#ifdef HAVE_LEVEL2
    Py_INCREF(&Entry_Type);
    if (PyDict_SetItemString(d, "Entry",
                             (PyObject *) &Entry_Type) < 0)
        return;

    Py_INCREF(&Permset_Type);
    if (PyDict_SetItemString(d, "Permset",
                             (PyObject *) &Permset_Type) < 0)
        return;

    /* 23.2.2 acl_perm_t values */
    PyModule_AddIntConstant(m, "ACL_READ", ACL_READ);
    PyModule_AddIntConstant(m, "ACL_WRITE", ACL_WRITE);
    PyModule_AddIntConstant(m, "ACL_EXECUTE", ACL_EXECUTE);

    /* 23.2.5 acl_tag_t values */
    PyModule_AddIntConstant(m, "ACL_UNDEFINED_TAG", ACL_UNDEFINED_TAG);
    PyModule_AddIntConstant(m, "ACL_USER_OBJ", ACL_USER_OBJ);
    PyModule_AddIntConstant(m, "ACL_USER", ACL_USER);
    PyModule_AddIntConstant(m, "ACL_GROUP_OBJ", ACL_GROUP_OBJ);
    PyModule_AddIntConstant(m, "ACL_GROUP", ACL_GROUP);
    PyModule_AddIntConstant(m, "ACL_MASK", ACL_MASK);
    PyModule_AddIntConstant(m, "ACL_OTHER", ACL_OTHER);

#endif
}
