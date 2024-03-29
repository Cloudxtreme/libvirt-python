/*
 * libvir.c: this modules implements the main part of the glue of the
 *           libvir library and the Python interpreter. It provides the
 *           entry points where an automatically generated stub is
 *           unpractical
 *
 * Copyright (C) 2005, 2007-2015 Red Hat, Inc.
 *
 * Daniel Veillard <veillard@redhat.com>
 */

/* Horrible kludge to work around even more horrible name-space pollution
   via Python.h.  That file includes /usr/include/python2.5/pyconfig*.h,
   which has over 180 autoconf-style HAVE_* definitions.  Shame on them.  */
#undef HAVE_PTHREAD_H

/* We want to see *_LAST enums.  */
#define VIR_ENUM_SENTINELS

#include <Python.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <stddef.h>
#include "typewrappers.h"
#include "build/libvirt.h"
#include "libvirt-utils.h"

#if PY_MAJOR_VERSION > 2
# ifndef __CYGWIN__
extern PyObject *PyInit_libvirtmod(void);
# else
extern PyObject *PyInit_cygvirtmod(void);
# endif
#else
# ifndef __CYGWIN__
extern void initlibvirtmod(void);
# else
extern void initcygvirtmod(void);
# endif
#endif

#if 0
# define DEBUG_ERROR 1
#endif

#if DEBUG_ERROR
# define DEBUG(fmt, ...)            \
   printf(fmt, __VA_ARGS__)
#else
# define DEBUG(fmt, ...)            \
   do {} while (0)
#endif

/* The two-statement sequence "Py_INCREF(Py_None); return Py_None;"
   is so common that we encapsulate it here.  Now, each use is simply
   return VIR_PY_NONE;  */
#define VIR_PY_NONE (Py_INCREF (Py_None), Py_None)
#define VIR_PY_INT_FAIL (libvirt_intWrap(-1))
#define VIR_PY_INT_SUCCESS (libvirt_intWrap(0))

static char *py_str(PyObject *obj)
{
    PyObject *str = PyObject_Str(obj);
    char *ret;
    if (!str) {
        PyErr_Print();
        PyErr_Clear();
        return NULL;
    };
    libvirt_charPtrUnwrap(str, &ret);
    return ret;
}

/* Helper function to convert a virTypedParameter output array into a
 * Python dictionary for return to the user.  Return NULL on failure,
 * after raising a python exception.  */
static PyObject *
getPyVirTypedParameter(const virTypedParameter *params, int nparams)
{
    PyObject *key, *val, *info;
    size_t i;

    if ((info = PyDict_New()) == NULL)
        return NULL;

    for (i = 0; i < nparams; i++) {
        switch (params[i].type) {
        case VIR_TYPED_PARAM_INT:
            val = libvirt_intWrap(params[i].value.i);
            break;

        case VIR_TYPED_PARAM_UINT:
            val = libvirt_intWrap(params[i].value.ui);
            break;

        case VIR_TYPED_PARAM_LLONG:
            val = libvirt_longlongWrap(params[i].value.l);
            break;

        case VIR_TYPED_PARAM_ULLONG:
            val = libvirt_ulonglongWrap(params[i].value.ul);
            break;

        case VIR_TYPED_PARAM_DOUBLE:
            val = PyFloat_FromDouble(params[i].value.d);
            break;

        case VIR_TYPED_PARAM_BOOLEAN:
            val = PyBool_FromLong(params[i].value.b);
            break;

        case VIR_TYPED_PARAM_STRING:
            val = libvirt_constcharPtrWrap(params[i].value.s);
            break;

        default:
            /* Possible if a newer server has a bug and sent stuff we
             * don't recognize.  */
            PyErr_Format(PyExc_LookupError,
                         "Type value \"%d\" not recognized",
                         params[i].type);
            val = NULL;
            break;
        }

        key = libvirt_constcharPtrWrap(params[i].field);
        if (!key || !val)
            goto cleanup;

        if (PyDict_SetItem(info, key, val) < 0) {
            Py_DECREF(info);
            goto cleanup;
        }

        Py_DECREF(key);
        Py_DECREF(val);
    }
    return info;

cleanup:
    Py_XDECREF(key);
    Py_XDECREF(val);
    return NULL;
}

/* Allocate a new typed parameter array with the same contents and
 * length as info, and using the array params of length nparams as
 * hints on what types to use when creating the new array. The caller
 * must clear the array before freeing it. Return NULL on failure,
 * after raising a python exception.  */
static virTypedParameterPtr ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2)
setPyVirTypedParameter(PyObject *info,
                       const virTypedParameter *params, int nparams)
{
    PyObject *key, *value;
#if PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION <= 4
    int pos = 0;
#else
    Py_ssize_t pos = 0;
#endif
    virTypedParameterPtr temp = NULL, ret = NULL;
    Py_ssize_t size;
    size_t i;

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    /* Libvirt APIs use NULL array and 0 size as a special case;
     * setting should have at least one parameter.  */
    if (size == 0) {
        PyErr_Format(PyExc_LookupError, "Dictionary must not be empty");
        return NULL;
    }

    if (VIR_ALLOC_N(ret, size) < 0) {
        PyErr_NoMemory();
        return NULL;
    }

    temp = &ret[0];
    while (PyDict_Next(info, &pos, &key, &value)) {
        char *keystr = NULL;

        if (libvirt_charPtrUnwrap(key, &keystr) < 0 ||
            keystr == NULL)
            goto cleanup;

        for (i = 0; i < nparams; i++) {
            if (STREQ(params[i].field, keystr))
                break;
        }
        if (i == nparams) {
            PyErr_Format(PyExc_LookupError,
                         "Attribute name \"%s\" could not be recognized",
                         keystr);
            VIR_FREE(keystr);
            goto cleanup;
        }

        strncpy(temp->field, keystr, VIR_TYPED_PARAM_FIELD_LENGTH - 1);
        temp->type = params[i].type;
        VIR_FREE(keystr);

        switch (params[i].type) {
        case VIR_TYPED_PARAM_INT:
            if (libvirt_intUnwrap(value, &temp->value.i) < 0)
                goto cleanup;
            break;

        case VIR_TYPED_PARAM_UINT:
            if (libvirt_uintUnwrap(value, &temp->value.ui) < 0)
                goto cleanup;
            break;

        case VIR_TYPED_PARAM_LLONG:
            if (libvirt_longlongUnwrap(value, &temp->value.l) < 0)
                goto cleanup;
            break;

        case VIR_TYPED_PARAM_ULLONG:
            if (libvirt_ulonglongUnwrap(value, &temp->value.ul) < 0)
                goto cleanup;
            break;

        case VIR_TYPED_PARAM_DOUBLE:
            if (libvirt_doubleUnwrap(value, &temp->value.d) < 0)
                goto cleanup;
            break;

        case VIR_TYPED_PARAM_BOOLEAN:
        {
            bool b;
            if (libvirt_boolUnwrap(value, &b) < 0)
                goto cleanup;
            temp->value.b = b;
            break;
        }
        case VIR_TYPED_PARAM_STRING:
        {
            char *string_val;
            if (libvirt_charPtrUnwrap(value, &string_val) < 0 ||
                !string_val)
                goto cleanup;
            temp->value.s = string_val;
            break;
        }

        default:
            /* Possible if a newer server has a bug and sent stuff we
             * don't recognize.  */
            PyErr_Format(PyExc_LookupError,
                         "Type value \"%d\" not recognized",
                         params[i].type);
            goto cleanup;
        }

        temp++;
    }
    return ret;

cleanup:
    virTypedParamsFree(ret, size);
    return NULL;
}

/* While these appeared in libvirt in 1.0.2, we only
 * need them in the python from 1.1.0 onwards */
#if LIBVIR_CHECK_VERSION(1, 1, 0)
typedef struct {
    const char *name;
    int type;
} virPyTypedParamsHint;
typedef virPyTypedParamsHint *virPyTypedParamsHintPtr;

# if PY_MAJOR_VERSION > 2
#  define libvirt_PyString_Check PyUnicode_Check
# else
#  define libvirt_PyString_Check PyString_Check
# endif

static int
virPyDictToTypedParamOne(virTypedParameterPtr *params,
                         int *n,
                         int *max,
                         virPyTypedParamsHintPtr hints,
                         int nhints,
                         const char *keystr,
                         PyObject *value)
{
    int rv = -1, type = -1;
    size_t i;

    for (i = 0; i < nhints; i++) {
        if (STREQ(hints[i].name, keystr)) {
            type = hints[i].type;
            break;
        }
    }

    if (type == -1) {
        if (libvirt_PyString_Check(value)) {
            type = VIR_TYPED_PARAM_STRING;
        } else if (PyBool_Check(value)) {
            type = VIR_TYPED_PARAM_BOOLEAN;
        } else if (PyLong_Check(value)) {
            unsigned long long ull = PyLong_AsUnsignedLongLong(value);
            if (ull == (unsigned long long) -1 && PyErr_Occurred())
                type = VIR_TYPED_PARAM_LLONG;
            else
                type = VIR_TYPED_PARAM_ULLONG;
#if PY_MAJOR_VERSION < 3
        } else if (PyInt_Check(value)) {
            if (PyInt_AS_LONG(value) < 0)
                type = VIR_TYPED_PARAM_LLONG;
            else
                type = VIR_TYPED_PARAM_ULLONG;
#endif
        } else if (PyFloat_Check(value)) {
            type = VIR_TYPED_PARAM_DOUBLE;
        }
    }

    if (type == -1) {
        PyErr_Format(PyExc_TypeError,
                     "Unknown type of \"%s\" field", keystr);
        goto cleanup;
    }

    switch ((virTypedParameterType) type) {
    case VIR_TYPED_PARAM_INT:
    {
        int val;
        if (libvirt_intUnwrap(value, &val) < 0 ||
            virTypedParamsAddInt(params, n, max, keystr, val) < 0)
            goto cleanup;
        break;
    }
    case VIR_TYPED_PARAM_UINT:
    {
        unsigned int val;
        if (libvirt_uintUnwrap(value, &val) < 0 ||
            virTypedParamsAddUInt(params, n, max, keystr, val) < 0)
            goto cleanup;
        break;
    }
    case VIR_TYPED_PARAM_LLONG:
    {
        long long val;
        if (libvirt_longlongUnwrap(value, &val) < 0 ||
            virTypedParamsAddLLong(params, n, max, keystr, val) < 0)
            goto cleanup;
        break;
    }
    case VIR_TYPED_PARAM_ULLONG:
    {
        unsigned long long val;
        if (libvirt_ulonglongUnwrap(value, &val) < 0 ||
            virTypedParamsAddULLong(params, n, max, keystr, val) < 0)
            goto cleanup;
        break;
    }
    case VIR_TYPED_PARAM_DOUBLE:
    {
        double val;
        if (libvirt_doubleUnwrap(value, &val) < 0 ||
            virTypedParamsAddDouble(params, n, max, keystr, val) < 0)
            goto cleanup;
        break;
    }
    case VIR_TYPED_PARAM_BOOLEAN:
    {
        bool val;
        if (libvirt_boolUnwrap(value, &val) < 0 ||
            virTypedParamsAddBoolean(params, n, max, keystr, val) < 0)
            goto cleanup;
        break;
    }
    case VIR_TYPED_PARAM_STRING:
    {
        char *val;;
        if (libvirt_charPtrUnwrap(value, &val) < 0 ||
            !val ||
            virTypedParamsAddString(params, n, max, keystr, val) < 0) {
            VIR_FREE(val);
            goto cleanup;
        }
        VIR_FREE(val);
        break;
    }
    case VIR_TYPED_PARAM_LAST:
        break; /* unreachable */
    }

    rv = 0;

 cleanup:
    return rv;
}


/* Automatically convert dict into type parameters based on types reported
 * by python. All integer types are converted into LLONG (in case of a negative
 * value) or ULLONG (in case of a positive value). If you need different
 * handling, use @hints to explicitly specify what types should be used for
 * specific parameters.
 */
static int
ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3)
virPyDictToTypedParams(PyObject *dict,
                       virTypedParameterPtr *ret_params,
                       int *ret_nparams,
                       virPyTypedParamsHintPtr hints,
                       int nhints)
{
    PyObject *key;
    PyObject *value;
#if PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION <= 4
    int pos = 0;
#else
    Py_ssize_t pos = 0;
#endif
    virTypedParameterPtr params = NULL;
    int n = 0;
    int max = 0;
    int ret = -1;
    char *keystr = NULL;

    *ret_params = NULL;
    *ret_nparams = 0;

    if (PyDict_Size(dict) < 0)
        return -1;

    while (PyDict_Next(dict, &pos, &key, &value)) {
        if (libvirt_charPtrUnwrap(key, &keystr) < 0 ||
            !keystr)
            goto cleanup;

        if (PyList_Check(value) || PyTuple_Check(value)) {
            Py_ssize_t i, size = PySequence_Size(value);

            for (i = 0; i < size; i++) {
                PyObject *v = PySequence_ITEM(value, i);
                if (virPyDictToTypedParamOne(&params, &n, &max,
                                             hints, nhints, keystr, v) < 0)
                    goto cleanup;
            }
        } else if (virPyDictToTypedParamOne(&params, &n, &max,
                                            hints, nhints, keystr, value) < 0)
            goto cleanup;

        VIR_FREE(keystr);
    }

    *ret_params = params;
    *ret_nparams = n;
    params = NULL;
    ret = 0;

cleanup:
    VIR_FREE(keystr);
    virTypedParamsFree(params, n);
    return ret;
}
#endif /* LIBVIR_CHECK_VERSION(1, 1, 0) */


/*
 * Utility function to retrieve the number of node CPUs present.
 * It first tries virNodeGetCPUMap, which will return the
 * number reliably, if available.
 * As a fallback and for compatibility with backlevel libvirt
 * versions virNodeGetInfo will be called to calculate the
 * CPU number, which has the potential to return a too small
 * number if some host CPUs are offline.
 */
static int
getPyNodeCPUCount(virConnectPtr conn) {
    int i_retval = -1;
    virNodeInfo nodeinfo;

#if LIBVIR_CHECK_VERSION(1, 0, 0)
    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeGetCPUMap(conn, NULL, NULL, 0);
    LIBVIRT_END_ALLOW_THREADS;
#endif /* LIBVIR_CHECK_VERSION(1, 0, 0) */

    if (i_retval < 0) {
        /* fallback: use nodeinfo */
        LIBVIRT_BEGIN_ALLOW_THREADS;
        i_retval = virNodeGetInfo(conn, &nodeinfo);
        LIBVIRT_END_ALLOW_THREADS;
        if (i_retval < 0)
            goto cleanup;

        i_retval = VIR_NODEINFO_MAXCPUS(nodeinfo);
    }

cleanup:
    return i_retval;
}

/************************************************************************
 *									*
 *		Statistics						*
 *									*
 ************************************************************************/

static PyObject *
libvirt_virDomainBlockStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    virDomainPtr domain;
    PyObject *pyobj_domain;
    char * path;
    int c_retval;
    virDomainBlockStatsStruct stats;
    PyObject *info;

    if (!PyArg_ParseTuple(args, (char *)"Oz:virDomainBlockStats",
        &pyobj_domain, &path))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainBlockStats(domain, path, &stats, sizeof(stats));
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    /* convert to a Python tuple of long objects */
    if ((info = PyTuple_New(5)) == NULL)
        return VIR_PY_NONE;
    PyTuple_SetItem(info, 0, libvirt_longlongWrap(stats.rd_req));
    PyTuple_SetItem(info, 1, libvirt_longlongWrap(stats.rd_bytes));
    PyTuple_SetItem(info, 2, libvirt_longlongWrap(stats.wr_req));
    PyTuple_SetItem(info, 3, libvirt_longlongWrap(stats.wr_bytes));
    PyTuple_SetItem(info, 4, libvirt_longlongWrap(stats.errs));
    return info;
}

static PyObject *
libvirt_virDomainBlockStatsFlags(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    virTypedParameterPtr params;
    const char *path;

    if (!PyArg_ParseTuple(args, (char *)"OzI:virDomainBlockStatsFlags",
                          &pyobj_domain, &path, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainBlockStatsFlags(domain, path, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainBlockStatsFlags(domain, path, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainGetCPUStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *totalbool;
    PyObject *cpu, *total;
    PyObject *ret = NULL;
    PyObject *error = NULL;
    int ncpus = -1, start_cpu = 0;
    int sumparams = 0, nparams = -1;
    size_t i;
    int i_retval;
    unsigned int flags;
    bool totalflag;
    virTypedParameterPtr params = NULL, cpuparams;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virDomainGetCPUStats",
                          &pyobj_domain, &totalbool, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (libvirt_boolUnwrap(totalbool, &totalflag) < 0)
        return NULL;

    if ((ret = PyList_New(0)) == NULL)
        return NULL;

    if (!totalflag) {
        LIBVIRT_BEGIN_ALLOW_THREADS;
        ncpus = virDomainGetCPUStats(domain, NULL, 0, 0, 0, flags);
        LIBVIRT_END_ALLOW_THREADS;

        if (ncpus < 0) {
            error = VIR_PY_NONE;
            goto error;
        }

        LIBVIRT_BEGIN_ALLOW_THREADS;
        nparams = virDomainGetCPUStats(domain, NULL, 0, 0, 1, flags);
        LIBVIRT_END_ALLOW_THREADS;

        if (nparams < 0) {
            error = VIR_PY_NONE;
            goto error;
        }

        sumparams = nparams * MIN(ncpus, 128);

        if (VIR_ALLOC_N(params, sumparams) < 0) {
            error = PyErr_NoMemory();
            goto error;
        }

        while (ncpus) {
            int queried_ncpus = MIN(ncpus, 128);
            if (nparams) {

                LIBVIRT_BEGIN_ALLOW_THREADS;
                i_retval = virDomainGetCPUStats(domain, params,
                                                nparams, start_cpu, queried_ncpus, flags);
                LIBVIRT_END_ALLOW_THREADS;

                if (i_retval < 0) {
                    error = VIR_PY_NONE;
                    goto error;
                }
            } else {
                i_retval = 0;
            }

            for (i = 0; i < queried_ncpus; i++) {
                cpuparams = &params[i * nparams];
                if ((cpu = getPyVirTypedParameter(cpuparams, i_retval)) == NULL) {
                    goto error;
                }

                if (PyList_Append(ret, cpu) < 0) {
                    Py_DECREF(cpu);
                    goto error;
                }
                Py_DECREF(cpu);
            }

            start_cpu += queried_ncpus;
            ncpus -= queried_ncpus;
            virTypedParamsClear(params, sumparams);
        }
    } else {
        LIBVIRT_BEGIN_ALLOW_THREADS;
        nparams = virDomainGetCPUStats(domain, NULL, 0, -1, 1, flags);
        LIBVIRT_END_ALLOW_THREADS;

        if (nparams < 0) {
            error = VIR_PY_NONE;
            goto error;
        }

        if (nparams) {
            sumparams = nparams;

            if (VIR_ALLOC_N(params, nparams) < 0) {
                error = PyErr_NoMemory();
                goto error;
            }

            LIBVIRT_BEGIN_ALLOW_THREADS;
            i_retval = virDomainGetCPUStats(domain, params, nparams, -1, 1, flags);
            LIBVIRT_END_ALLOW_THREADS;

            if (i_retval < 0) {
                error = VIR_PY_NONE;
                goto error;
            }
        } else {
            i_retval = 0;
        }

        if ((total = getPyVirTypedParameter(params, i_retval)) == NULL) {
            goto error;
        }
        if (PyList_Append(ret, total) < 0) {
            Py_DECREF(total);
            goto error;
        }
        Py_DECREF(total);
    }

    virTypedParamsFree(params, sumparams);
    return ret;

error:
    virTypedParamsFree(params, sumparams);
    Py_DECREF(ret);
    return error;
}

static PyObject *
libvirt_virDomainInterfaceStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    virDomainPtr domain;
    PyObject *pyobj_domain;
    char * path;
    int c_retval;
    virDomainInterfaceStatsStruct stats;
    PyObject *info;

    if (!PyArg_ParseTuple(args, (char *)"Oz:virDomainInterfaceStats",
        &pyobj_domain, &path))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainInterfaceStats(domain, path, &stats, sizeof(stats));
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    /* convert to a Python tuple of long objects */
    if ((info = PyTuple_New(8)) == NULL)
        return VIR_PY_NONE;
    PyTuple_SetItem(info, 0, libvirt_longlongWrap(stats.rx_bytes));
    PyTuple_SetItem(info, 1, libvirt_longlongWrap(stats.rx_packets));
    PyTuple_SetItem(info, 2, libvirt_longlongWrap(stats.rx_errs));
    PyTuple_SetItem(info, 3, libvirt_longlongWrap(stats.rx_drop));
    PyTuple_SetItem(info, 4, libvirt_longlongWrap(stats.tx_bytes));
    PyTuple_SetItem(info, 5, libvirt_longlongWrap(stats.tx_packets));
    PyTuple_SetItem(info, 6, libvirt_longlongWrap(stats.tx_errs));
    PyTuple_SetItem(info, 7, libvirt_longlongWrap(stats.tx_drop));
    return info;
}

static PyObject *
libvirt_virDomainMemoryStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    virDomainPtr domain;
    PyObject *pyobj_domain;
    unsigned int nr_stats;
    size_t i;
    virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
    PyObject *info;
    PyObject *key = NULL, *val = NULL;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainMemoryStats", &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    nr_stats = virDomainMemoryStats(domain, stats,
                                    VIR_DOMAIN_MEMORY_STAT_NR, 0);
    if (nr_stats == -1)
        return VIR_PY_NONE;

    /* convert to a Python dictionary */
    if ((info = PyDict_New()) == NULL)
        return VIR_PY_NONE;

    for (i = 0; i < nr_stats; i++) {
        switch (stats[i].tag) {
        case VIR_DOMAIN_MEMORY_STAT_SWAP_IN:
            key = libvirt_constcharPtrWrap("swap_in");
            break;
        case VIR_DOMAIN_MEMORY_STAT_SWAP_OUT:
            key = libvirt_constcharPtrWrap("swap_out");
            break;
        case VIR_DOMAIN_MEMORY_STAT_MAJOR_FAULT:
            key = libvirt_constcharPtrWrap("major_fault");
            break;
        case VIR_DOMAIN_MEMORY_STAT_MINOR_FAULT:
            key = libvirt_constcharPtrWrap("minor_fault");
            break;
        case VIR_DOMAIN_MEMORY_STAT_UNUSED:
            key = libvirt_constcharPtrWrap("unused");
            break;
        case VIR_DOMAIN_MEMORY_STAT_AVAILABLE:
            key = libvirt_constcharPtrWrap("available");
            break;
        case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
            key = libvirt_constcharPtrWrap("actual");
            break;
        case VIR_DOMAIN_MEMORY_STAT_RSS:
            key = libvirt_constcharPtrWrap("rss");
            break;
        default:
            continue;
        }
        val = libvirt_ulonglongWrap(stats[i].val);

        if (!key || !val || PyDict_SetItem(info, key, val) < 0) {
            Py_DECREF(info);
            info = NULL;
            goto cleanup;
        }
        Py_DECREF(key);
        Py_DECREF(val);
        key = NULL;
        val = NULL;
    }

 cleanup:
    Py_XDECREF(key);
    Py_XDECREF(val);
    return info;
}

static PyObject *
libvirt_virDomainGetSchedulerType(PyObject *self ATTRIBUTE_UNUSED,
                                  PyObject *args) {
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    char *c_retval;
    int nparams;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetScedulerType",
                          &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSchedulerType(domain, &nparams);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval == NULL)
        return VIR_PY_NONE;

    /* convert to a Python tuple of long objects */
    if ((info = PyTuple_New(2)) == NULL) {
        VIR_FREE(c_retval);
        return VIR_PY_NONE;
    }

    PyTuple_SetItem(info, 0, libvirt_constcharPtrWrap(c_retval));
    PyTuple_SetItem(info, 1, libvirt_intWrap((long)nparams));
    VIR_FREE(c_retval);
    return info;
}

static PyObject *
libvirt_virDomainGetSchedulerParameters(PyObject *self ATTRIBUTE_UNUSED,
                                        PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    char *c_retval;
    int i_retval;
    int nparams = 0;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetScedulerParameters",
                          &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSchedulerType(domain, &nparams);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == NULL)
        return VIR_PY_NONE;
    VIR_FREE(c_retval);

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetSchedulerParameters(domain, params, &nparams);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainGetSchedulerParametersFlags(PyObject *self ATTRIBUTE_UNUSED,
                                        PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    char *c_retval;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetScedulerParametersFlags",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSchedulerType(domain, &nparams);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == NULL)
        return VIR_PY_NONE;
    VIR_FREE(c_retval);

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetSchedulerParametersFlags(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainSetSchedulerParameters(PyObject *self ATTRIBUTE_UNUSED,
                                        PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    char *c_retval;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    virTypedParameterPtr params = NULL, new_params = NULL;

    if (!PyArg_ParseTuple(args, (char *)"OO:virDomainSetScedulerParameters",
                          &pyobj_domain, &info))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSchedulerType(domain, &nparams);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == NULL)
        return VIR_PY_INT_FAIL;
    VIR_FREE(c_retval);

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetSchedulerParameters(domain, params, &nparams);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetSchedulerParameters(domain, new_params, size);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, size);
    return ret;
}

static PyObject *
libvirt_virDomainSetSchedulerParametersFlags(PyObject *self ATTRIBUTE_UNUSED,
                                             PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    char *c_retval;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    unsigned int flags;
    virTypedParameterPtr params, new_params = NULL;

    if (!PyArg_ParseTuple(args,
                          (char *)"OOI:virDomainSetScedulerParametersFlags",
                          &pyobj_domain, &info, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSchedulerType(domain, &nparams);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == NULL)
        return VIR_PY_INT_FAIL;
    VIR_FREE(c_retval);

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetSchedulerParametersFlags(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetSchedulerParametersFlags(domain, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainSetBlkioParameters(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    unsigned int flags;
    virTypedParameterPtr params = NULL, new_params = NULL;

    if (!PyArg_ParseTuple(args,
                          (char *)"OOI:virDomainSetBlkioParameters",
                          &pyobj_domain, &info, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlkioParameters(domain, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlkioParameters(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetBlkioParameters(domain, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, size);
    return ret;
}

static PyObject *
libvirt_virDomainGetBlkioParameters(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetBlkioParameters",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlkioParameters(domain, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlkioParameters(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainSetMemoryParameters(PyObject *self ATTRIBUTE_UNUSED,
                                     PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    unsigned int flags;
    virTypedParameterPtr params = NULL, new_params = NULL;

    if (!PyArg_ParseTuple(args,
                          (char *)"OOI:virDomainSetMemoryParameters",
                          &pyobj_domain, &info, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetMemoryParameters(domain, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetMemoryParameters(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetMemoryParameters(domain, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, size);
    return ret;
}

static PyObject *
libvirt_virDomainGetMemoryParameters(PyObject *self ATTRIBUTE_UNUSED,
                                     PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetMemoryParameters",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetMemoryParameters(domain, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetMemoryParameters(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainSetNumaParameters(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    unsigned int flags;
    virTypedParameterPtr params = NULL, new_params = NULL;

    if (!PyArg_ParseTuple(args,
                          (char *)"OOI:virDomainSetNumaParameters",
                          &pyobj_domain, &info, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetNumaParameters(domain, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetNumaParameters(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetNumaParameters(domain, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, size);
    return ret;
}

static PyObject *
libvirt_virDomainGetNumaParameters(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetNumaParameters",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetNumaParameters(domain, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetNumaParameters(domain, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainSetInterfaceParameters(PyObject *self ATTRIBUTE_UNUSED,
                                        PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    unsigned int flags;
    const char *device = NULL;
    virTypedParameterPtr params = NULL, new_params = NULL;

    if (!PyArg_ParseTuple(args,
                          (char *)"OzOI:virDomainSetInterfaceParameters",
                          &pyobj_domain, &device, &info, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetInterfaceParameters(domain, device, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetInterfaceParameters(domain, device, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetInterfaceParameters(domain, device, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, size);
    return ret;
}

static PyObject *
libvirt_virDomainGetInterfaceParameters(PyObject *self ATTRIBUTE_UNUSED,
                                        PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    const char *device = NULL;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"Ozi:virDomainGetInterfaceParameters",
                          &pyobj_domain, &device, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetInterfaceParameters(domain, device, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetInterfaceParameters(domain, device, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainGetVcpus(PyObject *self ATTRIBUTE_UNUSED,
                          PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *pyretval = NULL, *pycpuinfo = NULL, *pycpumap = NULL;
    PyObject *error = NULL;
    virDomainInfo dominfo;
    virVcpuInfoPtr cpuinfo = NULL;
    unsigned char *cpumap = NULL;
    size_t cpumaplen, i;
    int i_retval, cpunum;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetVcpus",
                          &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_INT_FAIL;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetInfo(domain, &dominfo);
    LIBVIRT_END_ALLOW_THREADS;
    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (VIR_ALLOC_N(cpuinfo, dominfo.nrVirtCpu) < 0)
        return PyErr_NoMemory();

    cpumaplen = VIR_CPU_MAPLEN(cpunum);
    if (xalloc_oversized(dominfo.nrVirtCpu, cpumaplen) ||
        VIR_ALLOC_N(cpumap, dominfo.nrVirtCpu * cpumaplen) < 0) {
        error = PyErr_NoMemory();
        goto cleanup;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetVcpus(domain,
                                 cpuinfo, dominfo.nrVirtCpu,
                                 cpumap, cpumaplen);
    LIBVIRT_END_ALLOW_THREADS;
    if (i_retval < 0) {
        error = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    /* convert to a Python tuple of long objects */
    if ((pyretval = PyTuple_New(2)) == NULL)
        goto cleanup;
    if ((pycpuinfo = PyList_New(dominfo.nrVirtCpu)) == NULL)
        goto cleanup;
    if ((pycpumap = PyList_New(dominfo.nrVirtCpu)) == NULL)
        goto cleanup;

    for (i = 0; i < dominfo.nrVirtCpu; i++) {
        PyObject *info = PyTuple_New(4);
        PyObject *item = NULL;

        if (info == NULL)
            goto cleanup;

        if ((item = libvirt_intWrap((long)cpuinfo[i].number)) == NULL ||
            PyTuple_SetItem(info, 0, item) < 0)
            goto itemError;

        if ((item = libvirt_intWrap((long)cpuinfo[i].state)) == NULL ||
            PyTuple_SetItem(info, 1, item) < 0)
            goto itemError;

        if ((item = libvirt_ulonglongWrap(cpuinfo[i].cpuTime)) == NULL ||
            PyTuple_SetItem(info, 2, item) < 0)
            goto itemError;

        if ((item = libvirt_intWrap((long)cpuinfo[i].cpu)) == NULL ||
            PyTuple_SetItem(info, 3, item) < 0)
            goto itemError;

        if (PyList_SetItem(pycpuinfo, i, info) < 0)
            goto itemError;

        continue;
        itemError:
            Py_DECREF(info);
            Py_XDECREF(item);
            goto cleanup;
    }
    for (i = 0; i < dominfo.nrVirtCpu; i++) {
        PyObject *info = PyTuple_New(cpunum);
        size_t j;
        if (info == NULL)
            goto cleanup;
        for (j = 0; j < cpunum; j++) {
            PyObject *item = NULL;
            if ((item = PyBool_FromLong(VIR_CPU_USABLE(cpumap, cpumaplen, i, j))) == NULL ||
                PyTuple_SetItem(info, j, item) < 0) {
                Py_DECREF(info);
                Py_XDECREF(item);
                goto cleanup;
            }
        }
        if (PyList_SetItem(pycpumap, i, info) < 0) {
            Py_DECREF(info);
            goto cleanup;
        }
    }
    if (PyTuple_SetItem(pyretval, 0, pycpuinfo) < 0 ||
        PyTuple_SetItem(pyretval, 1, pycpumap) < 0)
        goto cleanup;

    VIR_FREE(cpuinfo);
    VIR_FREE(cpumap);

    return pyretval;

cleanup:
    VIR_FREE(cpuinfo);
    VIR_FREE(cpumap);
    Py_XDECREF(pyretval);
    Py_XDECREF(pycpuinfo);
    Py_XDECREF(pycpumap);
    return error;
}


static PyObject *
libvirt_virDomainPinVcpu(PyObject *self ATTRIBUTE_UNUSED,
                         PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *pycpumap;
    PyObject *ret = NULL;
    unsigned char *cpumap;
    int cpumaplen, vcpu, tuple_size, cpunum;
    size_t i;
    int i_retval;

    if (!PyArg_ParseTuple(args, (char *)"OiO:virDomainPinVcpu",
                          &pyobj_domain, &vcpu, &pycpumap))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_INT_FAIL;

    if (PyTuple_Check(pycpumap)) {
        tuple_size = PyTuple_Size(pycpumap);
        if (tuple_size == -1)
            return ret;
    } else {
       PyErr_SetString(PyExc_TypeError, "Unexpected type, tuple is required");
       return ret;
    }

    cpumaplen = VIR_CPU_MAPLEN(cpunum);
    if (VIR_ALLOC_N(cpumap, cpumaplen) < 0)
        return PyErr_NoMemory();

    for (i = 0; i < tuple_size; i++) {
        PyObject *flag = PyTuple_GetItem(pycpumap, i);
        bool b;

        if (!flag || libvirt_boolUnwrap(flag, &b) < 0)
            goto cleanup;

        if (b)
            VIR_USE_CPU(cpumap, i);
        else
            VIR_UNUSE_CPU(cpumap, i);
    }

    for (; i < cpunum; i++)
        VIR_UNUSE_CPU(cpumap, i);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainPinVcpu(domain, vcpu, cpumap, cpumaplen);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }
    ret = VIR_PY_INT_SUCCESS;

cleanup:
    VIR_FREE(cpumap);
    return ret;
}

static PyObject *
libvirt_virDomainPinVcpuFlags(PyObject *self ATTRIBUTE_UNUSED,
                              PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *pycpumap;
    PyObject *ret = NULL;
    unsigned char *cpumap;
    int cpumaplen, vcpu, tuple_size, cpunum;
    size_t i;
    unsigned int flags;
    int i_retval;

    if (!PyArg_ParseTuple(args, (char *)"OiOI:virDomainPinVcpuFlags",
                          &pyobj_domain, &vcpu, &pycpumap, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_INT_FAIL;

    if (PyTuple_Check(pycpumap)) {
        tuple_size = PyTuple_Size(pycpumap);
        if (tuple_size == -1)
            return ret;
    } else {
       PyErr_SetString(PyExc_TypeError, "Unexpected type, tuple is required");
       return ret;
    }

    cpumaplen = VIR_CPU_MAPLEN(cpunum);
    if (VIR_ALLOC_N(cpumap, cpumaplen) < 0)
        return PyErr_NoMemory();

    for (i = 0; i < tuple_size; i++) {
        PyObject *flag = PyTuple_GetItem(pycpumap, i);
        bool b;

        if (!flag || libvirt_boolUnwrap(flag, &b) < 0)
            goto cleanup;

        if (b)
            VIR_USE_CPU(cpumap, i);
        else
            VIR_UNUSE_CPU(cpumap, i);
    }

    for (; i < cpunum; i++)
        VIR_UNUSE_CPU(cpumap, i);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainPinVcpuFlags(domain, vcpu, cpumap, cpumaplen, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }
    ret = VIR_PY_INT_SUCCESS;

cleanup:
    VIR_FREE(cpumap);
    return ret;
}

static PyObject *
libvirt_virDomainGetVcpuPinInfo(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args) {
    virDomainPtr domain;
    PyObject *pyobj_domain, *pycpumaps = NULL;
    virDomainInfo dominfo;
    unsigned char *cpumaps = NULL;
    size_t cpumaplen, vcpu, pcpu;
    unsigned int flags;
    int i_retval, cpunum;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetVcpuPinInfo",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_INT_FAIL;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetInfo(domain, &dominfo);
    LIBVIRT_END_ALLOW_THREADS;
    if (i_retval < 0)
        return VIR_PY_NONE;

    cpumaplen = VIR_CPU_MAPLEN(cpunum);
    if (xalloc_oversized(dominfo.nrVirtCpu, cpumaplen) ||
        VIR_ALLOC_N(cpumaps, dominfo.nrVirtCpu * cpumaplen) < 0)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetVcpuPinInfo(domain, dominfo.nrVirtCpu,
                                       cpumaps, cpumaplen, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (i_retval < 0)
        goto cleanup;

    if ((pycpumaps = PyList_New(dominfo.nrVirtCpu)) == NULL)
        goto cleanup;

    for (vcpu = 0; vcpu < dominfo.nrVirtCpu; vcpu++) {
        PyObject *mapinfo = PyTuple_New(cpunum);
        if (mapinfo == NULL)
            goto cleanup;

        for (pcpu = 0; pcpu < cpunum; pcpu++) {
            PyTuple_SetItem(mapinfo, pcpu,
                            PyBool_FromLong(VIR_CPU_USABLE(cpumaps, cpumaplen, vcpu, pcpu)));
        }
        PyList_SetItem(pycpumaps, vcpu, mapinfo);
    }

    VIR_FREE(cpumaps);

    return pycpumaps;

cleanup:
    VIR_FREE(cpumaps);

    Py_XDECREF(pycpumaps);

    return VIR_PY_NONE;
}


#if LIBVIR_CHECK_VERSION(0, 10, 0)
static PyObject *
libvirt_virDomainPinEmulator(PyObject *self ATTRIBUTE_UNUSED,
                             PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *pycpumap;
    unsigned char *cpumap = NULL;
    int cpumaplen, tuple_size, cpunum;
    size_t i;
    int i_retval;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virDomainPinVcpu",
                          &pyobj_domain, &pycpumap, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_INT_FAIL;

    cpumaplen = VIR_CPU_MAPLEN(cpunum);

    if (!PyTuple_Check(pycpumap)) {
       PyErr_SetString(PyExc_TypeError, "Unexpected type, tuple is required");
       return NULL;
    }

    if ((tuple_size = PyTuple_Size(pycpumap)) == -1)
        return NULL;

    if (VIR_ALLOC_N(cpumap, cpumaplen) < 0)
        return PyErr_NoMemory();

    for (i = 0; i < tuple_size; i++) {
        PyObject *flag = PyTuple_GetItem(pycpumap, i);
        bool b;

        if (!flag || libvirt_boolUnwrap(flag, &b) < 0) {
            VIR_FREE(cpumap);
            return VIR_PY_INT_FAIL;
        }

        if (b)
            VIR_USE_CPU(cpumap, i);
        else
            VIR_UNUSE_CPU(cpumap, i);
    }

    for (; i < cpunum; i++)
        VIR_UNUSE_CPU(cpumap, i);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainPinEmulator(domain, cpumap, cpumaplen, flags);
    LIBVIRT_END_ALLOW_THREADS;

    VIR_FREE(cpumap);

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    return VIR_PY_INT_SUCCESS;
}


static PyObject *
libvirt_virDomainGetEmulatorPinInfo(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *pycpumap;
    unsigned char *cpumap;
    size_t cpumaplen;
    size_t pcpu;
    unsigned int flags;
    int ret;
    int cpunum;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainEmulatorPinInfo",
                          &pyobj_domain, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_NONE;

    cpumaplen = VIR_CPU_MAPLEN(cpunum);

    if (VIR_ALLOC_N(cpumap, cpumaplen) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virDomainGetEmulatorPinInfo(domain, cpumap, cpumaplen, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (ret < 0) {
        VIR_FREE(cpumap);
        return VIR_PY_NONE;
    }

    if (!(pycpumap = PyTuple_New(cpunum))) {
        VIR_FREE(cpumap);
        return NULL;
    }

    for (pcpu = 0; pcpu < cpunum; pcpu++)
        PyTuple_SET_ITEM(pycpumap, pcpu,
                         PyBool_FromLong(VIR_CPU_USABLE(cpumap, cpumaplen,
                                                        0, pcpu)));

    VIR_FREE(cpumap);
    return pycpumap;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 0) */

#if LIBVIR_CHECK_VERSION(1, 2, 14)
static PyObject *
libvirt_virDomainGetIOThreadInfo(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *py_retval = NULL;
    PyObject *py_iothrinfo = NULL;
    virDomainIOThreadInfoPtr *iothrinfo = NULL;
    unsigned int flags;
    size_t pcpu, i;
    int niothreads, cpunum;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetIOThreadInfo",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    niothreads = virDomainGetIOThreadInfo(domain, &iothrinfo, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (niothreads < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    /* convert to a Python list */
    if ((py_iothrinfo = PyList_New(niothreads)) == NULL)
        goto cleanup;

    /* NOTE: If there are zero IOThreads we will return an empty list */
    for (i = 0; i < niothreads; i++) {
        PyObject *iothrtpl = NULL;
        PyObject *iothrid = NULL;
        PyObject *iothrmap = NULL;
        virDomainIOThreadInfoPtr iothr = iothrinfo[i];

        if (iothr == NULL) {
            py_retval = VIR_PY_NONE;
            goto cleanup;
        }

        if ((iothrtpl = PyTuple_New(2)) == NULL ||
            PyList_SetItem(py_iothrinfo, i, iothrtpl) < 0) {
            Py_XDECREF(iothrtpl);
            goto cleanup;
        }

        /* 0: IOThread ID */
        if ((iothrid = libvirt_uintWrap(iothr->iothread_id)) == NULL ||
            PyTuple_SetItem(iothrtpl, 0, iothrid) < 0) {
            Py_XDECREF(iothrid);
            goto cleanup;
        }

        /* 1: CPU map */
        if ((iothrmap = PyList_New(cpunum)) == NULL ||
            PyTuple_SetItem(iothrtpl, 1, iothrmap) < 0) {
            Py_XDECREF(iothrmap);
            goto cleanup;
        }
        for (pcpu = 0; pcpu < cpunum; pcpu++) {
            PyObject *pyused;
            if ((pyused = PyBool_FromLong(VIR_CPU_USED(iothr->cpumap,
                                                       pcpu))) == NULL) {
                py_retval = VIR_PY_NONE;
                goto cleanup;
            }
            if (PyList_SetItem(iothrmap, pcpu, pyused) < 0) {
                Py_XDECREF(pyused);
                goto cleanup;
            }
        }
    }

    py_retval = py_iothrinfo;
    py_iothrinfo = NULL;

cleanup:
    if (niothreads > 0) {
        for (i = 0; i < niothreads; i++)
            virDomainIOThreadInfoFree(iothrinfo[i]);
    }
    VIR_FREE(iothrinfo);
    Py_XDECREF(py_iothrinfo);
    return py_retval;
}

static PyObject *
libvirt_virDomainPinIOThread(PyObject *self ATTRIBUTE_UNUSED,
                             PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *pycpumap;
    PyObject *ret = NULL;
    unsigned char *cpumap;
    int cpumaplen, iothread_val, tuple_size, cpunum;
    size_t i;
    unsigned int flags;
    int i_retval;

    if (!PyArg_ParseTuple(args, (char *)"OiOI:virDomainPinIOThread",
                          &pyobj_domain, &iothread_val, &pycpumap, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((cpunum = getPyNodeCPUCount(virDomainGetConnect(domain))) < 0)
        return VIR_PY_INT_FAIL;

    if (PyTuple_Check(pycpumap)) {
        if ((tuple_size = PyTuple_Size(pycpumap)) == -1)
            return ret;
    } else {
        PyErr_SetString(PyExc_TypeError, "Unexpected type, tuple is required");
        return ret;
    }

    cpumaplen = VIR_CPU_MAPLEN(cpunum);
    if (VIR_ALLOC_N(cpumap, cpumaplen) < 0)
        return PyErr_NoMemory();

    for (i = 0; i < tuple_size; i++) {
        PyObject *flag = PyTuple_GetItem(pycpumap, i);
        bool b;

        if (!flag || libvirt_boolUnwrap(flag, &b) < 0)
            goto cleanup;

        if (b)
            VIR_USE_CPU(cpumap, i);
        else
            VIR_UNUSE_CPU(cpumap, i);
    }

    for (; i < cpunum; i++)
        VIR_UNUSE_CPU(cpumap, i);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainPinIOThread(domain, iothread_val,
                                    cpumap, cpumaplen, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }
    ret = VIR_PY_INT_SUCCESS;

cleanup:
    VIR_FREE(cpumap);
    return ret;
}

#endif /* LIBVIR_CHECK_VERSION(1, 2, 14) */

/************************************************************************
 *									*
 *		Global error handler at the Python level		*
 *									*
 ************************************************************************/

static PyObject *libvirt_virPythonErrorFuncHandler = NULL;
static PyObject *libvirt_virPythonErrorFuncCtxt = NULL;

static PyObject *
libvirt_virGetLastError(PyObject *self ATTRIBUTE_UNUSED, PyObject *args ATTRIBUTE_UNUSED)
{
    virError *err;
    PyObject *info;

    if ((err = virGetLastError()) == NULL)
        return VIR_PY_NONE;

    if ((info = PyTuple_New(9)) == NULL)
        return VIR_PY_NONE;
    PyTuple_SetItem(info, 0, libvirt_intWrap((long) err->code));
    PyTuple_SetItem(info, 1, libvirt_intWrap((long) err->domain));
    PyTuple_SetItem(info, 2, libvirt_constcharPtrWrap(err->message));
    PyTuple_SetItem(info, 3, libvirt_intWrap((long) err->level));
    PyTuple_SetItem(info, 4, libvirt_constcharPtrWrap(err->str1));
    PyTuple_SetItem(info, 5, libvirt_constcharPtrWrap(err->str2));
    PyTuple_SetItem(info, 6, libvirt_constcharPtrWrap(err->str3));
    PyTuple_SetItem(info, 7, libvirt_intWrap((long) err->int1));
    PyTuple_SetItem(info, 8, libvirt_intWrap((long) err->int2));

    return info;
}

static PyObject *
libvirt_virConnGetLastError(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    virError *err;
    PyObject *info;
    virConnectPtr conn;
    PyObject *pyobj_conn;

    if (!PyArg_ParseTuple(args, (char *)"O:virConGetLastError", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    err = virConnGetLastError(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (err == NULL)
        return VIR_PY_NONE;

    if ((info = PyTuple_New(9)) == NULL)
        return VIR_PY_NONE;
    PyTuple_SetItem(info, 0, libvirt_intWrap((long) err->code));
    PyTuple_SetItem(info, 1, libvirt_intWrap((long) err->domain));
    PyTuple_SetItem(info, 2, libvirt_constcharPtrWrap(err->message));
    PyTuple_SetItem(info, 3, libvirt_intWrap((long) err->level));
    PyTuple_SetItem(info, 4, libvirt_constcharPtrWrap(err->str1));
    PyTuple_SetItem(info, 5, libvirt_constcharPtrWrap(err->str2));
    PyTuple_SetItem(info, 6, libvirt_constcharPtrWrap(err->str3));
    PyTuple_SetItem(info, 7, libvirt_intWrap((long) err->int1));
    PyTuple_SetItem(info, 8, libvirt_intWrap((long) err->int2));

    return info;
}

static void
libvirt_virErrorFuncHandler(ATTRIBUTE_UNUSED void *ctx, virErrorPtr err)
{
    PyObject *list, *info;
    PyObject *result;

    DEBUG("libvirt_virErrorFuncHandler(%p, %s, ...) called\n", ctx,
          err->message);

    if ((err == NULL) || (err->code == VIR_ERR_OK))
        return;

    LIBVIRT_ENSURE_THREAD_STATE;

    if ((libvirt_virPythonErrorFuncHandler == NULL) ||
        (libvirt_virPythonErrorFuncHandler == Py_None)) {
        virDefaultErrorFunc(err);
    } else {
        list = PyTuple_New(2);
        info = PyTuple_New(9);
        PyTuple_SetItem(list, 0, libvirt_virPythonErrorFuncCtxt);
        PyTuple_SetItem(list, 1, info);
        Py_XINCREF(libvirt_virPythonErrorFuncCtxt);
        PyTuple_SetItem(info, 0, libvirt_intWrap((long) err->code));
        PyTuple_SetItem(info, 1, libvirt_intWrap((long) err->domain));
        PyTuple_SetItem(info, 2, libvirt_constcharPtrWrap(err->message));
        PyTuple_SetItem(info, 3, libvirt_intWrap((long) err->level));
        PyTuple_SetItem(info, 4, libvirt_constcharPtrWrap(err->str1));
        PyTuple_SetItem(info, 5, libvirt_constcharPtrWrap(err->str2));
        PyTuple_SetItem(info, 6, libvirt_constcharPtrWrap(err->str3));
        PyTuple_SetItem(info, 7, libvirt_intWrap((long) err->int1));
        PyTuple_SetItem(info, 8, libvirt_intWrap((long) err->int2));
        /* TODO pass conn and dom if available */
        result = PyEval_CallObject(libvirt_virPythonErrorFuncHandler, list);
        Py_XDECREF(list);
        Py_XDECREF(result);
    }

    LIBVIRT_RELEASE_THREAD_STATE;
}

static PyObject *
libvirt_virRegisterErrorHandler(ATTRIBUTE_UNUSED PyObject * self,
                               PyObject * args)
{
    PyObject *py_retval;
    PyObject *pyobj_f;
    PyObject *pyobj_ctx;

    if (!PyArg_ParseTuple
        (args, (char *) "OO:xmlRegisterErrorHandler", &pyobj_f,
         &pyobj_ctx))
        return NULL;

    DEBUG("libvirt_virRegisterErrorHandler(%p, %p) called\n", pyobj_ctx,
          pyobj_f);

    virSetErrorFunc(NULL, libvirt_virErrorFuncHandler);
    if (libvirt_virPythonErrorFuncHandler != NULL) {
        Py_XDECREF(libvirt_virPythonErrorFuncHandler);
    }
    if (libvirt_virPythonErrorFuncCtxt != NULL) {
        Py_XDECREF(libvirt_virPythonErrorFuncCtxt);
    }

    if ((pyobj_f == Py_None) && (pyobj_ctx == Py_None)) {
        libvirt_virPythonErrorFuncHandler = NULL;
        libvirt_virPythonErrorFuncCtxt = NULL;
    } else {
        Py_XINCREF(pyobj_ctx);
        Py_XINCREF(pyobj_f);

        /* TODO: check f is a function ! */
        libvirt_virPythonErrorFuncHandler = pyobj_f;
        libvirt_virPythonErrorFuncCtxt = pyobj_ctx;
    }

    py_retval = libvirt_intWrap(1);
    return py_retval;
}

static int virConnectCredCallbackWrapper(virConnectCredentialPtr cred,
                                         unsigned int ncred,
                                         void *cbdata) {
    PyObject *list;
    PyObject *pycred;
    PyObject *pyauth = (PyObject *)cbdata;
    PyObject *pycbdata;
    PyObject *pycb;
    PyObject *pyret;
    int ret = -1;
    size_t i;

    LIBVIRT_ENSURE_THREAD_STATE;

    pycb = PyList_GetItem(pyauth, 1);
    pycbdata = PyList_GetItem(pyauth, 2);

    list = PyTuple_New(2);
    pycred = PyTuple_New(ncred);
    for (i = 0; i < ncred; i++) {
        PyObject *pycreditem;
        pycreditem = PyList_New(5);
        Py_INCREF(Py_None);
        PyTuple_SetItem(pycred, i, pycreditem);
        PyList_SetItem(pycreditem, 0, libvirt_intWrap((long) cred[i].type));
        PyList_SetItem(pycreditem, 1, libvirt_constcharPtrWrap(cred[i].prompt));
        if (cred[i].challenge) {
            PyList_SetItem(pycreditem, 2, libvirt_constcharPtrWrap(cred[i].challenge));
        } else {
            Py_INCREF(Py_None);
            PyList_SetItem(pycreditem, 2, Py_None);
        }
        if (cred[i].defresult) {
            PyList_SetItem(pycreditem, 3, libvirt_constcharPtrWrap(cred[i].defresult));
        } else {
            Py_INCREF(Py_None);
            PyList_SetItem(pycreditem, 3, Py_None);
        }
        PyList_SetItem(pycreditem, 4, Py_None);
    }

    PyTuple_SetItem(list, 0, pycred);
    Py_XINCREF(pycbdata);
    PyTuple_SetItem(list, 1, pycbdata);

    PyErr_Clear();
    pyret = PyEval_CallObject(pycb, list);
    if (PyErr_Occurred()) {
        PyErr_Print();
        goto cleanup;
    }

    ret = PyLong_AsLong(pyret);
    if (ret == 0) {
        for (i = 0; i < ncred; i++) {
            PyObject *pycreditem;
            PyObject *pyresult;
            char *result = NULL;
            pycreditem = PyTuple_GetItem(pycred, i);
            pyresult = PyList_GetItem(pycreditem, 4);
            if (pyresult != Py_None)
                libvirt_charPtrUnwrap(pyresult, &result);
            if (result != NULL) {
                cred[i].result = result;
                cred[i].resultlen = strlen(result);
            } else {
                cred[i].result = NULL;
                cred[i].resultlen = 0;
            }
        }
    }

 cleanup:
    Py_XDECREF(list);
    Py_XDECREF(pyret);

    LIBVIRT_RELEASE_THREAD_STATE;

    return ret;
}


static PyObject *
libvirt_virConnectOpenAuth(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    virConnectPtr c_retval;
    char * name;
    unsigned int flags;
    PyObject *pyauth;
    PyObject *pycredcb;
    PyObject *pycredtype;
    virConnectAuth auth;

    memset(&auth, 0, sizeof(auth));
    if (!PyArg_ParseTuple(args, (char *)"zOI:virConnectOpenAuth", &name, &pyauth, &flags))
        return NULL;

    pycredtype = PyList_GetItem(pyauth, 0);
    pycredcb = PyList_GetItem(pyauth, 1);

    auth.ncredtype = PyList_Size(pycredtype);
    if (auth.ncredtype) {
        size_t i;
        if (VIR_ALLOC_N(auth.credtype, auth.ncredtype) < 0)
            return VIR_PY_NONE;
        for (i = 0; i < auth.ncredtype; i++) {
            PyObject *val;
            val = PyList_GetItem(pycredtype, i);
            auth.credtype[i] = (int)PyLong_AsLong(val);
        }
    }
    if (pycredcb && pycredcb != Py_None)
        auth.cb = virConnectCredCallbackWrapper;
    auth.cbdata = pyauth;

    LIBVIRT_BEGIN_ALLOW_THREADS;

    c_retval = virConnectOpenAuth(name, &auth, flags);
    LIBVIRT_END_ALLOW_THREADS;
    VIR_FREE(auth.credtype);
    py_retval = libvirt_virConnectPtrWrap((virConnectPtr) c_retval);
    return py_retval;
}


/************************************************************************
 *									*
 *		Wrappers for functions where generator fails		*
 *									*
 ************************************************************************/

static PyObject *
libvirt_virGetVersion(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    char *type = NULL;
    unsigned long libVer, typeVer = 0;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *) "|s", &type))
        return NULL;

    LIBVIRT_BEGIN_ALLOW_THREADS;

    if (type == NULL)
        c_retval = virGetVersion(&libVer, NULL, NULL);
    else
        c_retval = virGetVersion(&libVer, type, &typeVer);

    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == -1)
        return VIR_PY_NONE;

    if (type == NULL)
        return libvirt_intWrap(libVer);
    else
        return Py_BuildValue((char *) "kk", libVer, typeVer);
}

static PyObject *
libvirt_virConnectGetVersion(PyObject *self ATTRIBUTE_UNUSED,
                             PyObject *args)
{
    unsigned long hvVersion;
    int c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;

    if (!PyArg_ParseTuple(args, (char *)"O:virConnectGetVersion",
                          &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    c_retval = virConnectGetVersion(conn, &hvVersion);

    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == -1)
        return VIR_PY_INT_FAIL;

    return libvirt_intWrap(hvVersion);
}

#if LIBVIR_CHECK_VERSION(1, 1, 3)
static PyObject *
libvirt_virConnectGetCPUModelNames(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    int c_retval;
    virConnectPtr conn;
    PyObject *rv = NULL, *pyobj_conn;
    char **models = NULL;
    size_t i;
    int flags = 0;
    const char *arch = NULL;

    if (!PyArg_ParseTuple(args, (char *)"OsI:virConnectGetCPUModelNames",
                          &pyobj_conn, &arch, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    c_retval = virConnectGetCPUModelNames(conn, arch, &models, flags);

    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == -1)
        return VIR_PY_NONE;

    if ((rv = PyList_New(c_retval)) == NULL)
        goto error;

    for (i = 0; i < c_retval; i++) {
        PyObject *str;
        if ((str = libvirt_constcharPtrWrap(models[i])) == NULL)
            goto error;

        PyList_SET_ITEM(rv, i, str);
    }

done:
    if (models) {
        for (i = 0; i < c_retval; i++)
            VIR_FREE(models[i]);
        VIR_FREE(models);
    }

    return rv;

error:
    Py_XDECREF(rv);
    rv = NULL;
    goto done;
}
#endif /* LIBVIR_CHECK_VERSION(1, 1, 3) */

static PyObject *
libvirt_virConnectGetLibVersion(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args)
{
    unsigned long libVer;
    int c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;

    if (!PyArg_ParseTuple(args, (char *)"O:virConnectGetLibVersion",
                          &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    c_retval = virConnectGetLibVersion(conn, &libVer);

    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == -1)
        return VIR_PY_INT_FAIL;

    return libvirt_intWrap(libVer);
}

static PyObject *
libvirt_virConnectListDomainsID(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args) {
    PyObject *py_retval;
    int *ids = NULL, c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListDomains", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfDomains(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(ids, c_retval) < 0)
            return VIR_PY_NONE;

        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListDomains(conn, ids, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(ids);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (ids) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_intWrap(ids[i]));
        }
        VIR_FREE(ids);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 9, 13)
static PyObject *
libvirt_virConnectListAllDomains(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virDomainPtr *doms = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllDomains",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllDomains(conn, &doms, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virDomainPtrWrap(doms[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        doms[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (doms[i])
            virDomainFree(doms[i]);
    VIR_FREE(doms);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 9, 13) */

static PyObject *
libvirt_virConnectListDefinedDomains(PyObject *self ATTRIBUTE_UNUSED,
                                     PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListDefinedDomains", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfDefinedDomains(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListDefinedDomains(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}

static PyObject *
libvirt_virDomainSnapshotListNames(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virDomainPtr dom;
    PyObject *pyobj_dom;
    PyObject *pyobj_snap;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainSnapshotListNames",
                          &pyobj_dom, &flags))
        return NULL;
    dom = (virDomainPtr) PyvirDomain_Get(pyobj_dom);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainSnapshotNum(dom, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return PyErr_NoMemory();
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virDomainSnapshotListNames(dom, names, c_retval, flags);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);
    if (!py_retval)
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if ((pyobj_snap = libvirt_constcharPtrWrap(names[i])) == NULL ||
            PyList_SetItem(py_retval, i, pyobj_snap) < 0) {
            Py_XDECREF(pyobj_snap);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        VIR_FREE(names[i]);
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        VIR_FREE(names[i]);
    VIR_FREE(names);
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 9, 13)
static PyObject *
libvirt_virDomainListAllSnapshots(PyObject *self ATTRIBUTE_UNUSED,
                                  PyObject *args)
{
    PyObject *py_retval = NULL;
    virDomainSnapshotPtr *snaps = NULL;
    int c_retval;
    size_t i;
    virDomainPtr dom;
    PyObject *pyobj_dom;
    unsigned int flags;
    PyObject *pyobj_snap;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainListAllSnapshots",
                          &pyobj_dom, &flags))
        return NULL;
    dom = (virDomainPtr) PyvirDomain_Get(pyobj_dom);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainListAllSnapshots(dom, &snaps, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if ((pyobj_snap = libvirt_virDomainSnapshotPtrWrap(snaps[i])) == NULL ||
            PyList_SetItem(py_retval, i, pyobj_snap) < 0) {
            Py_XDECREF(pyobj_snap);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        snaps[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (snaps[i])
            virDomainSnapshotFree(snaps[i]);
    VIR_FREE(snaps);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 9, 13) */

static PyObject *
libvirt_virDomainSnapshotListChildrenNames(PyObject *self ATTRIBUTE_UNUSED,
                                           PyObject *args)
{
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virDomainSnapshotPtr snap;
    PyObject *pyobj_snap;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainSnapshotListChildrenNames",
                          &pyobj_snap, &flags))
        return NULL;
    snap = (virDomainSnapshotPtr) PyvirDomainSnapshot_Get(pyobj_snap);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainSnapshotNumChildren(snap, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return PyErr_NoMemory();
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virDomainSnapshotListChildrenNames(snap, names, c_retval,
                                                      flags);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    for (i = 0; i < c_retval; i++) {
        if ((pyobj_snap = libvirt_constcharPtrWrap(names[i])) == NULL ||
            PyList_SetItem(py_retval, i, pyobj_snap) < 0) {
            Py_XDECREF(pyobj_snap);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        VIR_FREE(names[i]);
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        VIR_FREE(names[i]);
    VIR_FREE(names);
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 9, 13)
static PyObject *
libvirt_virDomainSnapshotListAllChildren(PyObject *self ATTRIBUTE_UNUSED,
                                         PyObject *args)
{
    PyObject *py_retval = NULL;
    virDomainSnapshotPtr *snaps = NULL;
    int c_retval;
    size_t i;
    virDomainSnapshotPtr parent;
    PyObject *pyobj_parent;
    unsigned int flags;
    PyObject *pyobj_snap;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainSnapshotListAllChildren",
                          &pyobj_parent, &flags))
        return NULL;
    parent = (virDomainSnapshotPtr) PyvirDomainSnapshot_Get(pyobj_parent);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainSnapshotListAllChildren(parent, &snaps, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if ((pyobj_snap = libvirt_virDomainSnapshotPtrWrap(snaps[i])) == NULL ||
            PyList_SetItem(py_retval, i, pyobj_snap) < 0) {
            Py_XDECREF(pyobj_snap);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        snaps[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (snaps[i])
            virDomainSnapshotFree(snaps[i]);
    VIR_FREE(snaps);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 9, 13) */

static PyObject *
libvirt_virDomainRevertToSnapshot(PyObject *self ATTRIBUTE_UNUSED,
                                  PyObject *args) {
    int c_retval;
    virDomainSnapshotPtr snap;
    PyObject *pyobj_snap;
    PyObject *pyobj_dom;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virDomainRevertToSnapshot", &pyobj_dom, &pyobj_snap, &flags))
        return NULL;
    snap = (virDomainSnapshotPtr) PyvirDomainSnapshot_Get(pyobj_snap);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainRevertToSnapshot(snap, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_INT_FAIL;

    return libvirt_intWrap(c_retval);
}

static PyObject *
libvirt_virDomainGetInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    virDomainInfo info;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetInfo", &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetInfo(domain, &info);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(5);
    PyList_SetItem(py_retval, 0, libvirt_intWrap((int) info.state));
    PyList_SetItem(py_retval, 1, libvirt_ulongWrap(info.maxMem));
    PyList_SetItem(py_retval, 2, libvirt_ulongWrap(info.memory));
    PyList_SetItem(py_retval, 3, libvirt_intWrap((int) info.nrVirtCpu));
    PyList_SetItem(py_retval, 4,
                   libvirt_ulonglongWrap(info.cpuTime));
    return py_retval;
}

static PyObject *
libvirt_virDomainGetState(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    PyObject *py_retval;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    int state;
    int reason;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetState",
                          &pyobj_domain, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetState(domain, &state, &reason, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = PyList_New(2);
    PyList_SetItem(py_retval, 0, libvirt_intWrap(state));
    PyList_SetItem(py_retval, 1, libvirt_intWrap(reason));
    return py_retval;
}

static PyObject *
libvirt_virDomainGetControlInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    virDomainControlInfo info;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainGetControlInfo",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetControlInfo(domain, &info, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(3);
    PyList_SetItem(py_retval, 0, libvirt_intWrap(info.state));
    PyList_SetItem(py_retval, 1, libvirt_intWrap(info.details));
    PyList_SetItem(py_retval, 2, libvirt_ulonglongWrap(info.stateTime));
    return py_retval;
}

static PyObject *
libvirt_virDomainGetBlockInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    virDomainBlockInfo info;
    const char *path;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OzI:virDomainGetInfo", &pyobj_domain, &path, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetBlockInfo(domain, path, &info, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(3);
    PyList_SetItem(py_retval, 0, libvirt_ulonglongWrap(info.capacity));
    PyList_SetItem(py_retval, 1, libvirt_ulonglongWrap(info.allocation));
    PyList_SetItem(py_retval, 2, libvirt_ulonglongWrap(info.physical));
    return py_retval;
}

static PyObject *
libvirt_virNodeGetInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    virNodeInfo info;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetInfo", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeGetInfo(conn, &info);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(8);
    PyList_SetItem(py_retval, 0, libvirt_constcharPtrWrap(&info.model[0]));
    PyList_SetItem(py_retval, 1, libvirt_longWrap((long) info.memory >> 10));
    PyList_SetItem(py_retval, 2, libvirt_intWrap((int) info.cpus));
    PyList_SetItem(py_retval, 3, libvirt_intWrap((int) info.mhz));
    PyList_SetItem(py_retval, 4, libvirt_intWrap((int) info.nodes));
    PyList_SetItem(py_retval, 5, libvirt_intWrap((int) info.sockets));
    PyList_SetItem(py_retval, 6, libvirt_intWrap((int) info.cores));
    PyList_SetItem(py_retval, 7, libvirt_intWrap((int) info.threads));
    return py_retval;
}

static PyObject *
libvirt_virNodeGetSecurityModel(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    virSecurityModel model;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetSecurityModel", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeGetSecurityModel(conn, &model);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(2);
    PyList_SetItem(py_retval, 0, libvirt_constcharPtrWrap(&model.model[0]));
    PyList_SetItem(py_retval, 1, libvirt_constcharPtrWrap(&model.doi[0]));
    return py_retval;
}

static PyObject *
libvirt_virDomainGetSecurityLabel(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virDomainPtr dom;
    PyObject *pyobj_dom;
    virSecurityLabel label;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetSecurityLabel", &pyobj_dom))
        return NULL;
    dom = (virDomainPtr) PyvirDomain_Get(pyobj_dom);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSecurityLabel(dom, &label);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(2);
    PyList_SetItem(py_retval, 0, libvirt_constcharPtrWrap(&label.label[0]));
    PyList_SetItem(py_retval, 1, libvirt_boolWrap(label.enforcing));
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 0)
static PyObject *
libvirt_virDomainGetSecurityLabelList(PyObject *self ATTRIBUTE_UNUSED,
                                      PyObject *args)
{
    PyObject *py_retval;
    int c_retval;
    virDomainPtr dom;
    PyObject *pyobj_dom;
    virSecurityLabel *labels = NULL;
    size_t i;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetSecurityLabel", &pyobj_dom))
        return NULL;

    dom = (virDomainPtr) PyvirDomain_Get(pyobj_dom);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetSecurityLabelList(dom, &labels);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(0)))
        goto error;

    for (i = 0 ; i < c_retval ; i++) {
        PyObject *entry;
        PyObject *value;

        if (!(entry = PyList_New(2)) ||
            PyList_Append(py_retval, entry) < 0) {
            Py_XDECREF(entry);
            goto error;
        }

        if (!(value = libvirt_constcharPtrWrap(&labels[i].label[0])) ||
            PyList_SetItem(entry, 0, value) < 0) {
            Py_XDECREF(value);
            goto error;
        }

        if (!(value = libvirt_boolWrap(labels[i].enforcing)) ||
            PyList_SetItem(entry, 1, value) < 0) {
            Py_XDECREF(value);
            goto error;
        }
    }

 cleanup:
    VIR_FREE(labels);
    return py_retval;

 error:
    Py_XDECREF(py_retval);
    py_retval = NULL;
    goto cleanup;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 0) */

static PyObject *
libvirt_virDomainGetUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virDomainPtr domain;
    PyObject *pyobj_domain;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetUUID", &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (domain == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetUUID(domain, &uuid[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = libvirt_charPtrSizeWrap((char *) &uuid[0], VIR_UUID_BUFLEN);

    return py_retval;
}

static PyObject *
libvirt_virDomainGetUUIDString(PyObject *self ATTRIBUTE_UNUSED,
                               PyObject *args) {
    PyObject *py_retval;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virDomainPtr dom;
    PyObject *pyobj_dom;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetUUIDString",
                          &pyobj_dom))
        return NULL;
    dom = (virDomainPtr) PyvirDomain_Get(pyobj_dom);

    if (dom == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetUUIDString(dom, &uuidstr[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_constcharPtrWrap((char *) &uuidstr[0]);
    return py_retval;
}

static PyObject *
libvirt_virDomainLookupByUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    virDomainPtr c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    unsigned char * uuid;
    int len;

    if (!PyArg_ParseTuple(args, (char *)"Oz#:virDomainLookupByUUID", &pyobj_conn, &uuid, &len))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((uuid == NULL) || (len != VIR_UUID_BUFLEN))
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainLookupByUUID(conn, uuid);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_virDomainPtrWrap((virDomainPtr) c_retval);
    return py_retval;
}


static PyObject *
libvirt_virConnectListNetworks(PyObject *self ATTRIBUTE_UNUSED,
                               PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListNetworks", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfNetworks(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListNetworks(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}


static PyObject *
libvirt_virConnectListDefinedNetworks(PyObject *self ATTRIBUTE_UNUSED,
                                      PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListDefinedNetworks", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfDefinedNetworks(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListDefinedNetworks(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virConnectListAllNetworks(PyObject *self ATTRIBUTE_UNUSED,
                                  PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virNetworkPtr *nets = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllNetworks",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllNetworks(conn, &nets, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virNetworkPtrWrap(nets[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        nets[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (nets[i])
            virNetworkFree(nets[i]);
    VIR_FREE(nets);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */


static PyObject *
libvirt_virNetworkGetUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virNetworkPtr domain;
    PyObject *pyobj_domain;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virNetworkGetUUID", &pyobj_domain))
        return NULL;
    domain = (virNetworkPtr) PyvirNetwork_Get(pyobj_domain);

    if (domain == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNetworkGetUUID(domain, &uuid[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = libvirt_charPtrSizeWrap((char *) &uuid[0], VIR_UUID_BUFLEN);

    return py_retval;
}

static PyObject *
libvirt_virNetworkGetUUIDString(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args) {
    PyObject *py_retval;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virNetworkPtr net;
    PyObject *pyobj_net;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virNetworkGetUUIDString",
                          &pyobj_net))
        return NULL;
    net = (virNetworkPtr) PyvirNetwork_Get(pyobj_net);

    if (net == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNetworkGetUUIDString(net, &uuidstr[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_constcharPtrWrap((char *) &uuidstr[0]);
    return py_retval;
}

static PyObject *
libvirt_virNetworkLookupByUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    virNetworkPtr c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    unsigned char * uuid;
    int len;

    if (!PyArg_ParseTuple(args, (char *)"Oz#:virNetworkLookupByUUID", &pyobj_conn, &uuid, &len))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((uuid == NULL) || (len != VIR_UUID_BUFLEN))
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNetworkLookupByUUID(conn, uuid);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_virNetworkPtrWrap((virNetworkPtr) c_retval);
    return py_retval;
}


static PyObject *
libvirt_virDomainGetAutostart(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval, autostart;
    virDomainPtr domain;
    PyObject *pyobj_domain;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetAutostart", &pyobj_domain))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetAutostart(domain, &autostart);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_INT_FAIL;
    py_retval = libvirt_intWrap(autostart);
    return py_retval;
}


static PyObject *
libvirt_virNetworkGetAutostart(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval, autostart;
    virNetworkPtr network;
    PyObject *pyobj_network;

    if (!PyArg_ParseTuple(args, (char *)"O:virNetworkGetAutostart", &pyobj_network))
        return NULL;

    network = (virNetworkPtr) PyvirNetwork_Get(pyobj_network);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNetworkGetAutostart(network, &autostart);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_INT_FAIL;
    py_retval = libvirt_intWrap(autostart);
    return py_retval;
}

static PyObject *
libvirt_virNodeGetCellsFreeMemory(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_conn;
    int startCell, maxCells, c_retval;
    size_t i;
    virConnectPtr conn;
    unsigned long long *freeMems;

    if (!PyArg_ParseTuple(args, (char *)"Oii:virNodeGetCellsFreeMemory", &pyobj_conn, &startCell, &maxCells))
        return NULL;

    if ((startCell < 0) || (maxCells <= 0) || (startCell + maxCells > 10000))
        return VIR_PY_NONE;

    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);
    if (VIR_ALLOC_N(freeMems, maxCells) < 0)
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeGetCellsFreeMemory(conn, freeMems, startCell, maxCells);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0) {
        VIR_FREE(freeMems);
        return VIR_PY_NONE;
    }
    py_retval = PyList_New(c_retval);
    for (i = 0; i < c_retval; i++) {
        PyList_SetItem(py_retval, i,
                libvirt_ulonglongWrap(freeMems[i]));
    }
    VIR_FREE(freeMems);
    return py_retval;
}

static PyObject *
libvirt_virNodeGetCPUStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    PyObject *ret = NULL;
    PyObject *key = NULL;
    PyObject *val = NULL;
    PyObject *pyobj_conn;
    virConnectPtr conn;
    unsigned int flags;
    int cpuNum, c_retval;
    size_t i;
    int nparams = 0;
    virNodeCPUStatsPtr stats = NULL;

    if (!PyArg_ParseTuple(args, (char *)"OiI:virNodeGetCPUStats", &pyobj_conn, &cpuNum, &flags))
        return ret;
    conn = (virConnectPtr)(PyvirConnect_Get(pyobj_conn));

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeGetCPUStats(conn, cpuNum, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (nparams) {
        if (VIR_ALLOC_N(stats, nparams) < 0)
            return PyErr_NoMemory();

        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virNodeGetCPUStats(conn, cpuNum, stats, &nparams, flags);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(stats);
            return VIR_PY_NONE;
        }
    }

    if (!(ret = PyDict_New()))
        goto error;

    for (i = 0; i < nparams; i++) {
        key = libvirt_constcharPtrWrap(stats[i].field);
        val = libvirt_ulonglongWrap(stats[i].value);

        if (!key || !val || PyDict_SetItem(ret, key, val) < 0) {
            Py_DECREF(ret);
            ret = NULL;
            goto error;
        }

        Py_DECREF(key);
        Py_DECREF(val);
    }

    VIR_FREE(stats);
    return ret;

error:
    VIR_FREE(stats);
    Py_XDECREF(key);
    Py_XDECREF(val);
    return ret;
}

static PyObject *
libvirt_virNodeGetMemoryStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    PyObject *ret = NULL;
    PyObject *key = NULL;
    PyObject *val = NULL;
    PyObject *pyobj_conn;
    virConnectPtr conn;
    unsigned int flags;
    int cellNum, c_retval;
    size_t i;
    int nparams = 0;
    virNodeMemoryStatsPtr stats = NULL;

    if (!PyArg_ParseTuple(args, (char *)"OiI:virNodeGetMemoryStats", &pyobj_conn, &cellNum, &flags))
        return ret;
    conn = (virConnectPtr)(PyvirConnect_Get(pyobj_conn));

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeGetMemoryStats(conn, cellNum, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (nparams) {
        if (VIR_ALLOC_N(stats, nparams) < 0)
            return PyErr_NoMemory();

        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virNodeGetMemoryStats(conn, cellNum, stats, &nparams, flags);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(stats);
            return VIR_PY_NONE;
        }
    }

    if (!(ret = PyDict_New()))
        goto error;

    for (i = 0; i < nparams; i++) {
        key = libvirt_constcharPtrWrap(stats[i].field);
        val = libvirt_ulonglongWrap(stats[i].value);

        if (!key || !val || PyDict_SetItem(ret, key, val) < 0) {
            Py_DECREF(ret);
            ret = NULL;
            goto error;
        }

        Py_DECREF(key);
        Py_DECREF(val);
    }

    VIR_FREE(stats);
    return ret;

error:
    VIR_FREE(stats);
    Py_XDECREF(key);
    Py_XDECREF(val);
    return ret;
}

static PyObject *
libvirt_virConnectListStoragePools(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListStoragePools", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfStoragePools(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListStoragePools(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);
    if (py_retval == NULL) {
        if (names) {
            for (i = 0; i < c_retval; i++)
                VIR_FREE(names[i]);
            VIR_FREE(names);
        }
        return VIR_PY_NONE;
    }

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}


static PyObject *
libvirt_virConnectListDefinedStoragePools(PyObject *self ATTRIBUTE_UNUSED,
                                          PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListDefinedStoragePools", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfDefinedStoragePools(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListDefinedStoragePools(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);
    if (py_retval == NULL) {
        if (names) {
            for (i = 0; i < c_retval; i++)
                VIR_FREE(names[i]);
            VIR_FREE(names);
        }
        return VIR_PY_NONE;
    }

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virConnectListAllStoragePools(PyObject *self ATTRIBUTE_UNUSED,
                                      PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virStoragePoolPtr *pools = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllStoragePools",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllStoragePools(conn, &pools, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virStoragePoolPtrWrap(pools[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        pools[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (pools[i])
            virStoragePoolFree(pools[i]);
    VIR_FREE(pools);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */

static PyObject *
libvirt_virStoragePoolListVolumes(PyObject *self ATTRIBUTE_UNUSED,
                                  PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virStoragePoolPtr pool;
    PyObject *pyobj_pool;


    if (!PyArg_ParseTuple(args, (char *)"O:virStoragePoolListVolumes", &pyobj_pool))
        return NULL;
    pool = (virStoragePoolPtr) PyvirStoragePool_Get(pyobj_pool);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolNumOfVolumes(pool);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virStoragePoolListVolumes(pool, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);
    if (py_retval == NULL) {
        if (names) {
            for (i = 0; i < c_retval; i++)
                VIR_FREE(names[i]);
            VIR_FREE(names);
        }
        return VIR_PY_NONE;
    }

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virStoragePoolListAllVolumes(PyObject *self ATTRIBUTE_UNUSED,
                                     PyObject *args)
{
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virStoragePoolPtr pool;
    virStorageVolPtr *vols = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;
    PyObject *pyobj_pool;

    if (!PyArg_ParseTuple(args, (char *)"OI:virStoragePoolListAllVolumes",
                          &pyobj_pool, &flags))
        return NULL;

    pool = (virStoragePoolPtr) PyvirStoragePool_Get(pyobj_pool);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolListAllVolumes(pool, &vols, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virStorageVolPtrWrap(vols[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        vols[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (vols[i])
            virStorageVolFree(vols[i]);
    VIR_FREE(vols);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */


static PyObject *
libvirt_virStoragePoolGetAutostart(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval, autostart;
    virStoragePoolPtr pool;
    PyObject *pyobj_pool;

    if (!PyArg_ParseTuple(args, (char *)"O:virStoragePoolGetAutostart", &pyobj_pool))
        return NULL;

    pool = (virStoragePoolPtr) PyvirStoragePool_Get(pyobj_pool);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolGetAutostart(pool, &autostart);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_intWrap(autostart);
    return py_retval;
}

static PyObject *
libvirt_virStoragePoolGetInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virStoragePoolPtr pool;
    PyObject *pyobj_pool;
    virStoragePoolInfo info;

    if (!PyArg_ParseTuple(args, (char *)"O:virStoragePoolGetInfo", &pyobj_pool))
        return NULL;
    pool = (virStoragePoolPtr) PyvirStoragePool_Get(pyobj_pool);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolGetInfo(pool, &info);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if ((py_retval = PyList_New(4)) == NULL)
        return VIR_PY_NONE;

    PyList_SetItem(py_retval, 0, libvirt_intWrap((int) info.state));
    PyList_SetItem(py_retval, 1,
                   libvirt_ulonglongWrap(info.capacity));
    PyList_SetItem(py_retval, 2,
                   libvirt_ulonglongWrap(info.allocation));
    PyList_SetItem(py_retval, 3,
                   libvirt_ulonglongWrap(info.available));
    return py_retval;
}


static PyObject *
libvirt_virStorageVolGetInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virStorageVolPtr pool;
    PyObject *pyobj_pool;
    virStorageVolInfo info;

    if (!PyArg_ParseTuple(args, (char *)"O:virStorageVolGetInfo", &pyobj_pool))
        return NULL;
    pool = (virStorageVolPtr) PyvirStorageVol_Get(pyobj_pool);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStorageVolGetInfo(pool, &info);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if ((py_retval = PyList_New(3)) == NULL)
        return VIR_PY_NONE;
    PyList_SetItem(py_retval, 0, libvirt_intWrap((int) info.type));
    PyList_SetItem(py_retval, 1,
                   libvirt_ulonglongWrap(info.capacity));
    PyList_SetItem(py_retval, 2,
                   libvirt_ulonglongWrap(info.allocation));
    return py_retval;
}

static PyObject *
libvirt_virStoragePoolGetUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virStoragePoolPtr pool;
    PyObject *pyobj_pool;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virStoragePoolGetUUID", &pyobj_pool))
        return NULL;
    pool = (virStoragePoolPtr) PyvirStoragePool_Get(pyobj_pool);

    if (pool == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolGetUUID(pool, &uuid[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_charPtrSizeWrap((char *) &uuid[0], VIR_UUID_BUFLEN);

    return py_retval;
}

static PyObject *
libvirt_virStoragePoolGetUUIDString(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args) {
    PyObject *py_retval;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virStoragePoolPtr pool;
    PyObject *pyobj_pool;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virStoragePoolGetUUIDString", &pyobj_pool))
        return NULL;
    pool = (virStoragePoolPtr) PyvirStoragePool_Get(pyobj_pool);

    if (pool == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolGetUUIDString(pool, &uuidstr[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_constcharPtrWrap((char *) &uuidstr[0]);
    return py_retval;
}

static PyObject *
libvirt_virStoragePoolLookupByUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    virStoragePoolPtr c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    unsigned char * uuid;
    int len;

    if (!PyArg_ParseTuple(args, (char *)"Oz#:virStoragePoolLookupByUUID", &pyobj_conn, &uuid, &len))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((uuid == NULL) || (len != VIR_UUID_BUFLEN))
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virStoragePoolLookupByUUID(conn, uuid);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_virStoragePoolPtrWrap((virStoragePoolPtr) c_retval);
    return py_retval;
}

static PyObject *
libvirt_virNodeListDevices(PyObject *self ATTRIBUTE_UNUSED,
                           PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    char *cap;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OzI:virNodeListDevices",
                          &pyobj_conn, &cap, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeNumOfDevices(conn, cap, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virNodeListDevices(conn, cap, names, c_retval, flags);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virConnectListAllNodeDevices(PyObject *self ATTRIBUTE_UNUSED,
                                     PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virNodeDevicePtr *devices = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllNodeDevices",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllNodeDevices(conn, &devices, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virNodeDevicePtrWrap(devices[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        devices[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (devices[i])
            virNodeDeviceFree(devices[i]);
    VIR_FREE(devices);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */

static PyObject *
libvirt_virNodeDeviceListCaps(PyObject *self ATTRIBUTE_UNUSED,
                              PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virNodeDevicePtr dev;
    PyObject *pyobj_dev;

    if (!PyArg_ParseTuple(args, (char *)"O:virNodeDeviceListCaps", &pyobj_dev))
        return NULL;
    dev = (virNodeDevicePtr) PyvirNodeDevice_Get(pyobj_dev);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeDeviceNumOfCaps(dev);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virNodeDeviceListCaps(dev, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}

static PyObject *
libvirt_virSecretGetUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virSecretPtr secret;
    PyObject *pyobj_secret;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virSecretGetUUID", &pyobj_secret))
        return NULL;
    secret = (virSecretPtr) PyvirSecret_Get(pyobj_secret);

    if (secret == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virSecretGetUUID(secret, &uuid[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = libvirt_charPtrSizeWrap((char *) &uuid[0], VIR_UUID_BUFLEN);

    return py_retval;
}

static PyObject *
libvirt_virSecretGetUUIDString(PyObject *self ATTRIBUTE_UNUSED,
                               PyObject *args) {
    PyObject *py_retval;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virSecretPtr dom;
    PyObject *pyobj_dom;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virSecretGetUUIDString",
                          &pyobj_dom))
        return NULL;
    dom = (virSecretPtr) PyvirSecret_Get(pyobj_dom);

    if (dom == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virSecretGetUUIDString(dom, &uuidstr[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_constcharPtrWrap((char *) &uuidstr[0]);
    return py_retval;
}

static PyObject *
libvirt_virSecretLookupByUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    virSecretPtr c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    unsigned char * uuid;
    int len;

    if (!PyArg_ParseTuple(args, (char *)"Oz#:virSecretLookupByUUID", &pyobj_conn, &uuid, &len))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((uuid == NULL) || (len != VIR_UUID_BUFLEN))
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virSecretLookupByUUID(conn, uuid);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_virSecretPtrWrap((virSecretPtr) c_retval);
    return py_retval;
}


static PyObject *
libvirt_virConnectListSecrets(PyObject *self ATTRIBUTE_UNUSED,
                              PyObject *args) {
    PyObject *py_retval;
    char **uuids = NULL;
    virConnectPtr conn;
    int c_retval;
    size_t i;
    PyObject *pyobj_conn;

    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListSecrets", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfSecrets(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(uuids, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListSecrets(conn, uuids, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(uuids);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (uuids) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(uuids[i]));
            VIR_FREE(uuids[i]);
        }
        VIR_FREE(uuids);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virConnectListAllSecrets(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virSecretPtr *secrets = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllSecrets",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllSecrets(conn, &secrets, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virSecretPtrWrap(secrets[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        secrets[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (secrets[i])
            virSecretFree(secrets[i]);
    VIR_FREE(secrets);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */

static PyObject *
libvirt_virSecretGetValue(PyObject *self ATTRIBUTE_UNUSED,
                          PyObject *args) {
    PyObject *py_retval;
    unsigned char *c_retval;
    size_t size;
    virSecretPtr secret;
    PyObject *pyobj_secret;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virSecretGetValue", &pyobj_secret,
                          &flags))
        return NULL;
    secret = (virSecretPtr) PyvirSecret_Get(pyobj_secret);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virSecretGetValue(secret, &size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval == NULL)
        return VIR_PY_NONE;

    py_retval = libvirt_charPtrSizeWrap((char*)c_retval, size);
    VIR_FREE(c_retval);

    return py_retval;
}

static PyObject *
libvirt_virSecretSetValue(PyObject *self ATTRIBUTE_UNUSED,
                          PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virSecretPtr secret;
    PyObject *pyobj_secret;
    const char *value;
    int size;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"Oz#I:virSecretSetValue", &pyobj_secret,
                          &value, &size, &flags))
        return NULL;
    secret = (virSecretPtr) PyvirSecret_Get(pyobj_secret);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virSecretSetValue(secret, (const unsigned char *)value, size,
                                 flags);
    LIBVIRT_END_ALLOW_THREADS;

    py_retval = libvirt_intWrap(c_retval);
    return py_retval;
}

static PyObject *
libvirt_virNWFilterGetUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virNWFilterPtr nwfilter;
    PyObject *pyobj_nwfilter;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virNWFilterGetUUID", &pyobj_nwfilter))
        return NULL;
    nwfilter = (virNWFilterPtr) PyvirNWFilter_Get(pyobj_nwfilter);

    if (nwfilter == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNWFilterGetUUID(nwfilter, &uuid[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = libvirt_charPtrSizeWrap((char *) &uuid[0], VIR_UUID_BUFLEN);

    return py_retval;
}

static PyObject *
libvirt_virNWFilterGetUUIDString(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args) {
    PyObject *py_retval;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virNWFilterPtr nwfilter;
    PyObject *pyobj_nwfilter;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"O:virNWFilterGetUUIDString",
                          &pyobj_nwfilter))
        return NULL;
    nwfilter = (virNWFilterPtr) PyvirNWFilter_Get(pyobj_nwfilter);

    if (nwfilter == NULL)
        return VIR_PY_NONE;
    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNWFilterGetUUIDString(nwfilter, &uuidstr[0]);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_NONE;

    py_retval = libvirt_constcharPtrWrap((char *) &uuidstr[0]);
    return py_retval;
}

static PyObject *
libvirt_virNWFilterLookupByUUID(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    virNWFilterPtr c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    unsigned char * uuid;
    int len;

    if (!PyArg_ParseTuple(args, (char *)"Oz#:virNWFilterLookupByUUID", &pyobj_conn, &uuid, &len))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((uuid == NULL) || (len != VIR_UUID_BUFLEN))
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNWFilterLookupByUUID(conn, uuid);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_virNWFilterPtrWrap((virNWFilterPtr) c_retval);
    return py_retval;
}


static PyObject *
libvirt_virConnectListNWFilters(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args) {
    PyObject *py_retval;
    char **uuids = NULL;
    virConnectPtr conn;
    int c_retval;
    size_t i;
    PyObject *pyobj_conn;

    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListNWFilters", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfNWFilters(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(uuids, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListNWFilters(conn, uuids, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(uuids);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);

    if (uuids) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(uuids[i]));
            VIR_FREE(uuids[i]);
        }
        VIR_FREE(uuids);
    }

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virConnectListAllNWFilters(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virNWFilterPtr *filters = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllNWFilters",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllNWFilters(conn, &filters, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virNWFilterPtrWrap(filters[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        filters[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (filters[i])
            virNWFilterFree(filters[i]);
    VIR_FREE(filters);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */

static PyObject *
libvirt_virConnectListInterfaces(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListInterfaces", &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfInterfaces(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListInterfaces(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);
    if (py_retval == NULL) {
        if (names) {
            for (i = 0; i < c_retval; i++)
                VIR_FREE(names[i]);
            VIR_FREE(names);
        }
        return VIR_PY_NONE;
    }

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}


static PyObject *
libvirt_virConnectListDefinedInterfaces(PyObject *self ATTRIBUTE_UNUSED,
                                        PyObject *args) {
    PyObject *py_retval;
    char **names = NULL;
    int c_retval;
    size_t i;
    virConnectPtr conn;
    PyObject *pyobj_conn;


    if (!PyArg_ParseTuple(args, (char *)"O:virConnectListDefinedInterfaces",
                          &pyobj_conn))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectNumOfDefinedInterfaces(conn);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (c_retval) {
        if (VIR_ALLOC_N(names, c_retval) < 0)
            return VIR_PY_NONE;
        LIBVIRT_BEGIN_ALLOW_THREADS;
        c_retval = virConnectListDefinedInterfaces(conn, names, c_retval);
        LIBVIRT_END_ALLOW_THREADS;
        if (c_retval < 0) {
            VIR_FREE(names);
            return VIR_PY_NONE;
        }
    }
    py_retval = PyList_New(c_retval);
    if (py_retval == NULL) {
        if (names) {
            for (i = 0; i < c_retval; i++)
                VIR_FREE(names[i]);
            VIR_FREE(names);
        }
        return VIR_PY_NONE;
    }

    if (names) {
        for (i = 0; i < c_retval; i++) {
            PyList_SetItem(py_retval, i, libvirt_constcharPtrWrap(names[i]));
            VIR_FREE(names[i]);
        }
        VIR_FREE(names);
    }

    return py_retval;
}


#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virConnectListAllInterfaces(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval = NULL;
    PyObject *tmp = NULL;
    virConnectPtr conn;
    virInterfacePtr *ifaces = NULL;
    int c_retval = 0;
    size_t i;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OI:virConnectListAllInterfaces",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virConnectListAllInterfaces(conn, &ifaces, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;

    if (!(py_retval = PyList_New(c_retval)))
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        if (!(tmp = libvirt_virInterfacePtrWrap(ifaces[i])) ||
            PyList_SetItem(py_retval, i, tmp) < 0) {
            Py_XDECREF(tmp);
            Py_DECREF(py_retval);
            py_retval = NULL;
            goto cleanup;
        }
        /* python steals the pointer */
        ifaces[i] = NULL;
    }

cleanup:
    for (i = 0; i < c_retval; i++)
        if (ifaces[i])
            virInterfaceFree(ifaces[i]);
    VIR_FREE(ifaces);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */

static PyObject *
libvirt_virConnectBaselineCPU(PyObject *self ATTRIBUTE_UNUSED,
                              PyObject *args) {
    PyObject *pyobj_conn;
    PyObject *list;
    virConnectPtr conn;
    unsigned int flags;
    char **xmlcpus = NULL;
    int ncpus = 0;
    char *base_cpu;
    PyObject *pybase_cpu;
    size_t i, j;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virConnectBaselineCPU",
                          &pyobj_conn, &list, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if (PyList_Check(list)) {
        ncpus = PyList_Size(list);
        if (VIR_ALLOC_N(xmlcpus, ncpus) < 0)
            return VIR_PY_NONE;

        for (i = 0; i < ncpus; i++) {
            if (libvirt_charPtrUnwrap(PyList_GetItem(list, i), &(xmlcpus[i])) < 0 ||
                xmlcpus[i] == NULL) {
                for (j = 0 ; j < i ; j++)
                    VIR_FREE(xmlcpus[j]);
                VIR_FREE(xmlcpus);
                return VIR_PY_NONE;
            }
        }
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    base_cpu = virConnectBaselineCPU(conn, (const char **)xmlcpus, ncpus, flags);
    LIBVIRT_END_ALLOW_THREADS;

    for (i = 0 ; i < ncpus ; i++)
        VIR_FREE(xmlcpus[i]);
    VIR_FREE(xmlcpus);

    if (base_cpu == NULL)
        return VIR_PY_NONE;

    pybase_cpu = libvirt_constcharPtrWrap(base_cpu);
    VIR_FREE(base_cpu);

    if (pybase_cpu == NULL)
        return VIR_PY_NONE;

    return pybase_cpu;
}


static PyObject *
libvirt_virDomainGetJobInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    virDomainJobInfo info;

    if (!PyArg_ParseTuple(args, (char *)"O:virDomainGetJobInfo", &pyobj_domain))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetJobInfo(domain, &info);
    LIBVIRT_END_ALLOW_THREADS;
    if (c_retval < 0)
        return VIR_PY_NONE;
    py_retval = PyList_New(12);
    PyList_SetItem(py_retval, 0, libvirt_intWrap((int) info.type));
    PyList_SetItem(py_retval, 1, libvirt_ulonglongWrap(info.timeElapsed));
    PyList_SetItem(py_retval, 2, libvirt_ulonglongWrap(info.timeRemaining));
    PyList_SetItem(py_retval, 3, libvirt_ulonglongWrap(info.dataTotal));
    PyList_SetItem(py_retval, 4, libvirt_ulonglongWrap(info.dataProcessed));
    PyList_SetItem(py_retval, 5, libvirt_ulonglongWrap(info.dataRemaining));
    PyList_SetItem(py_retval, 6, libvirt_ulonglongWrap(info.memTotal));
    PyList_SetItem(py_retval, 7, libvirt_ulonglongWrap(info.memProcessed));
    PyList_SetItem(py_retval, 8, libvirt_ulonglongWrap(info.memRemaining));
    PyList_SetItem(py_retval, 9, libvirt_ulonglongWrap(info.fileTotal));
    PyList_SetItem(py_retval, 10, libvirt_ulonglongWrap(info.fileProcessed));
    PyList_SetItem(py_retval, 11, libvirt_ulonglongWrap(info.fileRemaining));

    return py_retval;
}

#if LIBVIR_CHECK_VERSION(1, 0, 3)
static PyObject *
libvirt_virDomainGetJobStats(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    PyObject *pyobj_domain;
    virDomainPtr domain;
    unsigned int flags;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int type;
    PyObject *dict = NULL;
    int rc;

    if (!PyArg_ParseTuple(args, (char *) "OI:virDomainGetJobStats",
                          &pyobj_domain, &flags))
        goto cleanup;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    rc = virDomainGetJobStats(domain, &type, &params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;
    if (rc < 0)
        goto cleanup;

    if (!(dict = getPyVirTypedParameter(params, nparams)))
        goto cleanup;

    if (PyDict_SetItem(dict, libvirt_constcharPtrWrap("type"),
                       libvirt_intWrap(type)) < 0) {
        Py_DECREF(dict);
        dict = NULL;
        goto cleanup;
    }

cleanup:
    virTypedParamsFree(params, nparams);
    if (dict)
        return dict;
    else
        return VIR_PY_NONE;
}
#endif /* LIBVIR_CHECK_VERSION(1, 0, 3) */

static PyObject *
libvirt_virDomainGetBlockJobInfo(PyObject *self ATTRIBUTE_UNUSED,
                                 PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    const char *path;
    unsigned int flags;
    virDomainBlockJobInfo info;
    int c_ret;
    PyObject *type = NULL, *bandwidth = NULL, *cur = NULL, *end = NULL;
    PyObject *dict;

    if (!PyArg_ParseTuple(args, (char *)"OzI:virDomainGetBlockJobInfo",
                          &pyobj_domain, &path, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((dict = PyDict_New()) == NULL)
        return NULL;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_ret = virDomainGetBlockJobInfo(domain, path, &info, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_ret == 0) {
        return dict;
    } else if (c_ret < 0) {
        Py_DECREF(dict);
        return VIR_PY_NONE;
    }

    if ((type = libvirt_intWrap(info.type)) == NULL ||
        PyDict_SetItemString(dict, "type", type) < 0)
        goto error;
    Py_DECREF(type);

    if ((bandwidth = libvirt_ulongWrap(info.bandwidth)) == NULL ||
        PyDict_SetItemString(dict, "bandwidth", bandwidth) < 0)
        goto error;
    Py_DECREF(bandwidth);

    if ((cur = libvirt_ulonglongWrap(info.cur)) == NULL ||
        PyDict_SetItemString(dict, "cur", cur) < 0)
        goto error;
    Py_DECREF(cur);

    if ((end = libvirt_ulonglongWrap(info.end)) == NULL ||
        PyDict_SetItemString(dict, "end", end) < 0)
        goto error;
    Py_DECREF(end);

    return dict;

error:
    Py_DECREF(dict);
    Py_XDECREF(type);
    Py_XDECREF(bandwidth);
    Py_XDECREF(cur);
    Py_XDECREF(end);
    return NULL;
}

static PyObject *
libvirt_virDomainSetBlockIoTune(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain, *info;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    const char *disk;
    unsigned int flags;
    virTypedParameterPtr params = NULL, new_params = NULL;

    if (!PyArg_ParseTuple(args, (char *)"OzOI:virDomainSetBlockIoTune",
                          &pyobj_domain, &disk, &info, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlockIoTune(domain, disk, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Domain has no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlockIoTune(domain, disk, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainSetBlockIoTune(domain, disk, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, size);
    return ret;
}

static PyObject *
libvirt_virDomainGetBlockIoTune(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args)
{
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    const char *disk;
    unsigned int flags;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"OzI:virDomainGetBlockIoTune",
                          &pyobj_domain, &disk, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlockIoTune(domain, disk, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virDomainGetBlockIoTune(domain, disk, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}

static PyObject *
libvirt_virDomainGetDiskErrors(PyObject *self ATTRIBUTE_UNUSED,
                               PyObject *args)
{
    PyObject *py_retval = VIR_PY_NONE;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    unsigned int flags;
    virDomainDiskErrorPtr disks = NULL;
    unsigned int ndisks;
    int count;
    size_t i;

    if (!PyArg_ParseTuple(args, (char *) "OI:virDomainGetDiskErrors",
                          &pyobj_domain, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if ((count = virDomainGetDiskErrors(domain, NULL, 0, 0)) < 0)
        return VIR_PY_NONE;
    ndisks = count;

    if (ndisks) {
        if (VIR_ALLOC_N(disks, ndisks) < 0)
            return VIR_PY_NONE;

        LIBVIRT_BEGIN_ALLOW_THREADS;
        count = virDomainGetDiskErrors(domain, disks, ndisks, 0);
        LIBVIRT_END_ALLOW_THREADS;

        if (count < 0)
            goto cleanup;
    }

    if (!(py_retval = PyDict_New()))
        goto cleanup;

    for (i = 0; i < count; i++) {
        PyDict_SetItem(py_retval,
                       libvirt_constcharPtrWrap(disks[i].disk),
                       libvirt_intWrap(disks[i].error));
    }

cleanup:
    if (disks) {
        for (i = 0; i < count; i++)
            VIR_FREE(disks[i].disk);
        VIR_FREE(disks);
    }
    return py_retval;
}


#if LIBVIR_CHECK_VERSION(1, 2, 14)
static PyObject *
libvirt_virDomainInterfaceAddresses(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    PyObject *py_retval = VIR_PY_NONE;
    PyObject *pyobj_domain;
    virDomainPtr domain;
    virDomainInterfacePtr *ifaces = NULL;
    unsigned int source;
    unsigned int flags;
    int ifaces_count = 0;
    size_t i, j;

    if (!PyArg_ParseTuple(args, (char *) "Oii:virDomainInterfaceAddresses",
                          &pyobj_domain, &source, &flags))
        goto error;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ifaces_count = virDomainInterfaceAddresses(domain, &ifaces, source, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (ifaces_count < 0)
        goto cleanup;

    if (!(py_retval = PyDict_New()))
        goto error;

    for (i = 0; i < ifaces_count; i++) {
        virDomainInterfacePtr iface = ifaces[i];
        PyObject *py_addrs = NULL;
        PyObject *py_iface = NULL;
        PyObject *py_iname = NULL;
        PyObject *py_ivalue = NULL;

        if (!(py_iface = PyDict_New()))
            goto error;

        if ((py_iname = libvirt_charPtrWrap(iface->name)) == NULL ||
            PyDict_SetItem(py_retval, py_iname, py_iface) < 0) {
            Py_XDECREF(py_iname);
            Py_DECREF(py_iface);
            goto error;
        }

        if (iface->naddrs) {
            if (!(py_addrs = PyList_New(iface->naddrs))) {
                goto error;
            }
        } else {
            py_addrs = VIR_PY_NONE;
        }

        if ((py_iname = libvirt_constcharPtrWrap("addrs")) == NULL ||
            PyDict_SetItem(py_iface, py_iname, py_addrs) < 0) {
            Py_XDECREF(py_iname);
            Py_DECREF(py_addrs);
            goto error;
        }

        if ((py_iname = libvirt_constcharPtrWrap("hwaddr")) == NULL ||
            (py_ivalue = libvirt_constcharPtrWrap(iface->hwaddr)) == NULL ||
            PyDict_SetItem(py_iface, py_iname, py_ivalue) < 0) {
            Py_XDECREF(py_iname);
            Py_XDECREF(py_ivalue);
            goto error;
        }

        for (j = 0; j < iface->naddrs; j++) {
            virDomainIPAddressPtr addr = &(iface->addrs[j]);
            PyObject *py_addr = PyDict_New();

            if (!py_addr)
                goto error;

            if (PyList_SetItem(py_addrs, j, py_addr) < 0) {
                Py_DECREF(py_addr);
                goto error;
            }

            if ((py_iname = libvirt_constcharPtrWrap("addr")) == NULL ||
                (py_ivalue = libvirt_constcharPtrWrap(addr->addr)) == NULL ||
                PyDict_SetItem(py_addr, py_iname, py_ivalue) < 0) {
                Py_XDECREF(py_iname);
                Py_XDECREF(py_ivalue);
                goto error;
            }
            if ((py_iname = libvirt_constcharPtrWrap("prefix")) == NULL ||
                (py_ivalue = libvirt_intWrap(addr->prefix)) == NULL ||
                PyDict_SetItem(py_addr, py_iname, py_ivalue) < 0) {
                Py_XDECREF(py_iname);
                Py_XDECREF(py_ivalue);
                goto error;
            }
            if ((py_iname = libvirt_constcharPtrWrap("type")) == NULL ||
                (py_ivalue = libvirt_intWrap(addr->type)) == NULL ||
                PyDict_SetItem(py_addr, py_iname, py_ivalue) < 0) {
                Py_XDECREF(py_iname);
                Py_XDECREF(py_ivalue);
                goto error;
            }
        }
    }

cleanup:
    if (ifaces && ifaces_count > 0) {
        for (i = 0; i < ifaces_count; i++) {
            virDomainInterfaceFree(ifaces[i]);
        }
    }
    VIR_FREE(ifaces);

    return py_retval;

error:
    Py_XDECREF(py_retval);
    py_retval = NULL;
    goto cleanup;
}
#endif /* LIBVIR_CHECK_VERSION(1, 2, 14) */


/*******************************************
 * Helper functions to avoid importing modules
 * for every callback
 *******************************************/
static PyObject *libvirt_module    = NULL;
static PyObject *libvirt_dict      = NULL;

static PyObject *
getLibvirtModuleObject(void) {
    if (libvirt_module)
        return libvirt_module;

    // PyImport_ImportModule returns a new reference
    /* Bogus (char *) cast for RHEL-5 python API brokenness */
    libvirt_module = PyImport_ImportModule((char *)"libvirt");
    if (!libvirt_module) {
        DEBUG("%s Error importing libvirt module\n", __FUNCTION__);
        PyErr_Print();
        return NULL;
    }

    return libvirt_module;
}

static PyObject *
getLibvirtDictObject(void) {
    if (libvirt_dict)
        return libvirt_dict;

    // PyModule_GetDict returns a borrowed reference
    libvirt_dict = PyModule_GetDict(getLibvirtModuleObject());
    if (!libvirt_dict) {
        DEBUG("%s Error importing libvirt dictionary\n", __FUNCTION__);
        PyErr_Print();
        return NULL;
    }

    Py_INCREF(libvirt_dict);
    return libvirt_dict;
}


static PyObject *
libvirt_lookupPythonFunc(const char *funcname)
{
    PyObject *python_cb;

    /* Lookup the python callback */
    python_cb = PyDict_GetItemString(getLibvirtDictObject(), funcname);

    if (!python_cb) {
        DEBUG("%s: Error finding %s\n", __FUNCTION__, funcname);
        PyErr_Print();
        PyErr_Clear();
        return NULL;
    }

    if (!PyCallable_Check(python_cb)) {
        DEBUG("%s: %s is not callable\n", __FUNCTION__, funcname);
        return NULL;
    }

    return python_cb;
}

/*******************************************
 * Domain Events
 *******************************************/

static int
libvirt_virConnectDomainEventCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                      virDomainPtr dom,
                                      int event,
                                      int detail,
                                      void *opaque)
{
    PyObject *pyobj_ret = NULL;

    PyObject *pyobj_conn = (PyObject*)opaque;
    PyObject *pyobj_dom;

    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventCallbacks",
                                    (char*)"Oii",
                                    pyobj_dom,
                                    event, detail);

    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static PyObject *
libvirt_virConnectDomainEventRegister(ATTRIBUTE_UNUSED PyObject *self,
                                      PyObject *args)
{
    PyObject *py_retval;        /* return value */
    PyObject *pyobj_conn;       /* virConnectPtr */
    PyObject *pyobj_conn_inst;  /* virConnect Python object */

    virConnectPtr conn;
    int ret = 0;

    if (!PyArg_ParseTuple
        (args, (char *) "OO:virConnectDomainEventRegister",
                        &pyobj_conn, &pyobj_conn_inst)) {
        DEBUG("%s failed parsing tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    DEBUG("libvirt_virConnectDomainEventRegister(%p %p) called\n",
          pyobj_conn, pyobj_conn_inst);
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    Py_INCREF(pyobj_conn_inst);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    ret = virConnectDomainEventRegister(conn,
                                        libvirt_virConnectDomainEventCallback,
                                        pyobj_conn_inst, NULL);

    LIBVIRT_END_ALLOW_THREADS;

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static PyObject *
libvirt_virConnectDomainEventDeregister(ATTRIBUTE_UNUSED PyObject *self,
                                        PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_conn;
    PyObject *pyobj_conn_inst;

    virConnectPtr conn;
    int ret = 0;

    if (!PyArg_ParseTuple
        (args, (char *) "OO:virConnectDomainEventDeregister",
         &pyobj_conn, &pyobj_conn_inst))
        return NULL;

    DEBUG("libvirt_virConnectDomainEventDeregister(%p) called\n", pyobj_conn);

    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    ret = virConnectDomainEventDeregister(conn, libvirt_virConnectDomainEventCallback);

    LIBVIRT_END_ALLOW_THREADS;

    Py_DECREF(pyobj_conn_inst);
    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

/*******************************************
 * Event Impl
 *******************************************/
static PyObject *addHandleObj;
static char *addHandleName;
static PyObject *updateHandleObj;
static char *updateHandleName;
static PyObject *removeHandleObj;
static char *removeHandleName;
static PyObject *addTimeoutObj;
static char *addTimeoutName;
static PyObject *updateTimeoutObj;
static char *updateTimeoutName;
static PyObject *removeTimeoutObj;
static char *removeTimeoutName;

#define NAME(fn) ( fn ## Name ? fn ## Name : # fn )

static int
libvirt_virEventAddHandleFunc  (int fd,
                                int event,
                                virEventHandleCallback cb,
                                void *opaque,
                                virFreeCallback ff)
{
    PyObject *result;
    PyObject *python_cb;
    PyObject *cb_obj;
    PyObject *ff_obj;
    PyObject *opaque_obj;
    PyObject *cb_args;
    PyObject *pyobj_args;
    int retval = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    /* Lookup the python callback */
    python_cb = libvirt_lookupPythonFunc("_eventInvokeHandleCallback");
    if (!python_cb) {
        goto cleanup;
    }
    Py_INCREF(python_cb);

    /* create tuple for cb */
    cb_obj = libvirt_virEventHandleCallbackWrap(cb);
    ff_obj = libvirt_virFreeCallbackWrap(ff);
    opaque_obj = libvirt_virVoidPtrWrap(opaque);

    cb_args = PyTuple_New(3);
    PyTuple_SetItem(cb_args, 0, cb_obj);
    PyTuple_SetItem(cb_args, 1, opaque_obj);
    PyTuple_SetItem(cb_args, 2, ff_obj);

    pyobj_args = PyTuple_New(4);
    PyTuple_SetItem(pyobj_args, 0, libvirt_intWrap(fd));
    PyTuple_SetItem(pyobj_args, 1, libvirt_intWrap(event));
    PyTuple_SetItem(pyobj_args, 2, python_cb);
    PyTuple_SetItem(pyobj_args, 3, cb_args);

    result = PyEval_CallObject(addHandleObj, pyobj_args);
    if (!result) {
        PyErr_Print();
        PyErr_Clear();
    } else {
        libvirt_intUnwrap(result, &retval);
    }

    Py_XDECREF(result);
    Py_DECREF(pyobj_args);

cleanup:
    LIBVIRT_RELEASE_THREAD_STATE;

    return retval;
}

static void
libvirt_virEventUpdateHandleFunc(int watch, int event)
{
    PyObject *result;
    PyObject *pyobj_args;

    LIBVIRT_ENSURE_THREAD_STATE;

    pyobj_args = PyTuple_New(2);
    PyTuple_SetItem(pyobj_args, 0, libvirt_intWrap(watch));
    PyTuple_SetItem(pyobj_args, 1, libvirt_intWrap(event));

    result = PyEval_CallObject(updateHandleObj, pyobj_args);
    if (!result) {
        PyErr_Print();
        PyErr_Clear();
    }

    Py_XDECREF(result);
    Py_DECREF(pyobj_args);

    LIBVIRT_RELEASE_THREAD_STATE;
}


static int
libvirt_virEventRemoveHandleFunc(int watch)
{
    PyObject *result;
    PyObject *pyobj_args;
    PyObject *opaque;
    PyObject *ff;
    int retval = -1;
    virFreeCallback cff;

    LIBVIRT_ENSURE_THREAD_STATE;

    pyobj_args = PyTuple_New(1);
    PyTuple_SetItem(pyobj_args, 0, libvirt_intWrap(watch));

    result = PyEval_CallObject(removeHandleObj, pyobj_args);
    if (!result) {
        PyErr_Print();
        PyErr_Clear();
    } else if (!PyTuple_Check(result) || PyTuple_Size(result) != 3) {
        DEBUG("%s: %s must return opaque obj registered with %s"
              "to avoid leaking libvirt memory\n",
              __FUNCTION__, NAME(removeHandle), NAME(addHandle));
    } else {
        opaque = PyTuple_GetItem(result, 1);
        ff = PyTuple_GetItem(result, 2);
        cff = PyvirFreeCallback_Get(ff);
        if (cff)
            (*cff)(PyvirVoidPtr_Get(opaque));
        retval = 0;
    }

    Py_XDECREF(result);
    Py_DECREF(pyobj_args);

    LIBVIRT_RELEASE_THREAD_STATE;

    return retval;
}


static int
libvirt_virEventAddTimeoutFunc(int timeout,
                               virEventTimeoutCallback cb,
                               void *opaque,
                               virFreeCallback ff)
{
    PyObject *result;

    PyObject *python_cb;

    PyObject *cb_obj;
    PyObject *ff_obj;
    PyObject *opaque_obj;
    PyObject *cb_args;
    PyObject *pyobj_args;
    int retval = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    /* Lookup the python callback */
    python_cb = libvirt_lookupPythonFunc("_eventInvokeTimeoutCallback");
    if (!python_cb) {
        goto cleanup;
    }
    Py_INCREF(python_cb);

    /* create tuple for cb */
    cb_obj = libvirt_virEventTimeoutCallbackWrap(cb);
    ff_obj = libvirt_virFreeCallbackWrap(ff);
    opaque_obj = libvirt_virVoidPtrWrap(opaque);

    cb_args = PyTuple_New(3);
    PyTuple_SetItem(cb_args, 0, cb_obj);
    PyTuple_SetItem(cb_args, 1, opaque_obj);
    PyTuple_SetItem(cb_args, 2, ff_obj);

    pyobj_args = PyTuple_New(3);

    PyTuple_SetItem(pyobj_args, 0, libvirt_intWrap(timeout));
    PyTuple_SetItem(pyobj_args, 1, python_cb);
    PyTuple_SetItem(pyobj_args, 2, cb_args);

    result = PyEval_CallObject(addTimeoutObj, pyobj_args);
    if (!result) {
        PyErr_Print();
        PyErr_Clear();
    } else {
        libvirt_intUnwrap(result, &retval);
    }

    Py_XDECREF(result);
    Py_DECREF(pyobj_args);

cleanup:
    LIBVIRT_RELEASE_THREAD_STATE;
    return retval;
}

static void
libvirt_virEventUpdateTimeoutFunc(int timer, int timeout)
{
    PyObject *result = NULL;
    PyObject *pyobj_args;

    LIBVIRT_ENSURE_THREAD_STATE;

    pyobj_args = PyTuple_New(2);
    PyTuple_SetItem(pyobj_args, 0, libvirt_intWrap(timer));
    PyTuple_SetItem(pyobj_args, 1, libvirt_intWrap(timeout));

    result = PyEval_CallObject(updateTimeoutObj, pyobj_args);
    if (!result) {
        PyErr_Print();
        PyErr_Clear();
    }

    Py_XDECREF(result);
    Py_DECREF(pyobj_args);

    LIBVIRT_RELEASE_THREAD_STATE;
}

static int
libvirt_virEventRemoveTimeoutFunc(int timer)
{
    PyObject *result = NULL;
    PyObject *pyobj_args;
    PyObject *opaque;
    PyObject *ff;
    int retval = -1;
    virFreeCallback cff;

    LIBVIRT_ENSURE_THREAD_STATE;

    pyobj_args = PyTuple_New(1);
    PyTuple_SetItem(pyobj_args, 0, libvirt_intWrap(timer));

    result = PyEval_CallObject(removeTimeoutObj, pyobj_args);
    if (!result) {
        PyErr_Print();
        PyErr_Clear();
    } else if (!PyTuple_Check(result) || PyTuple_Size(result) != 3) {
        DEBUG("%s: %s must return opaque obj registered with %s"
              "to avoid leaking libvirt memory\n",
              __FUNCTION__, NAME(removeTimeout), NAME(addTimeout));
    } else {
        opaque = PyTuple_GetItem(result, 1);
        ff = PyTuple_GetItem(result, 2);
        cff = PyvirFreeCallback_Get(ff);
        if (cff)
            (*cff)(PyvirVoidPtr_Get(opaque));
        retval = 0;
    }

    Py_XDECREF(result);
    Py_DECREF(pyobj_args);

    LIBVIRT_RELEASE_THREAD_STATE;

    return retval;
}

static PyObject *
libvirt_virEventRegisterImpl(ATTRIBUTE_UNUSED PyObject * self,
                             PyObject * args)
{
    /* Unref the previously-registered impl (if any) */
    Py_XDECREF(addHandleObj);
    Py_XDECREF(updateHandleObj);
    Py_XDECREF(removeHandleObj);
    Py_XDECREF(addTimeoutObj);
    Py_XDECREF(updateTimeoutObj);
    Py_XDECREF(removeTimeoutObj);
    VIR_FREE(addHandleName);
    VIR_FREE(updateHandleName);
    VIR_FREE(removeHandleName);
    VIR_FREE(addTimeoutName);
    VIR_FREE(updateTimeoutName);
    VIR_FREE(removeTimeoutName);

    /* Parse and check arguments */
    if (!PyArg_ParseTuple(args, (char *) "OOOOOO:virEventRegisterImpl",
                          &addHandleObj, &updateHandleObj,
                          &removeHandleObj, &addTimeoutObj,
                          &updateTimeoutObj, &removeTimeoutObj) ||
        !PyCallable_Check(addHandleObj) ||
        !PyCallable_Check(updateHandleObj) ||
        !PyCallable_Check(removeHandleObj) ||
        !PyCallable_Check(addTimeoutObj) ||
        !PyCallable_Check(updateTimeoutObj) ||
        !PyCallable_Check(removeTimeoutObj))
        return VIR_PY_INT_FAIL;

    /* Get argument string representations (for error reporting) */
    addHandleName = py_str(addHandleObj);
    updateHandleName = py_str(updateHandleObj);
    removeHandleName = py_str(removeHandleObj);
    addTimeoutName = py_str(addTimeoutObj);
    updateTimeoutName = py_str(updateTimeoutObj);
    removeTimeoutName = py_str(removeTimeoutObj);

    /* Inc refs since we're holding on to these objects until
     * the next call (if any) to this function.
     */
    Py_INCREF(addHandleObj);
    Py_INCREF(updateHandleObj);
    Py_INCREF(removeHandleObj);
    Py_INCREF(addTimeoutObj);
    Py_INCREF(updateTimeoutObj);
    Py_INCREF(removeTimeoutObj);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    /* Now register our C EventImpl, which will dispatch
     * to the Python callbacks passed in as args.
     */
    virEventRegisterImpl(libvirt_virEventAddHandleFunc,
                         libvirt_virEventUpdateHandleFunc,
                         libvirt_virEventRemoveHandleFunc,
                         libvirt_virEventAddTimeoutFunc,
                         libvirt_virEventUpdateTimeoutFunc,
                         libvirt_virEventRemoveTimeoutFunc);

    LIBVIRT_END_ALLOW_THREADS;

    return VIR_PY_INT_SUCCESS;
}

static PyObject *
libvirt_virEventInvokeHandleCallback(PyObject *self ATTRIBUTE_UNUSED,
                                     PyObject *args)
{
    int watch, fd, event;
    PyObject *py_f;
    PyObject *py_opaque;
    virEventHandleCallback cb;
    void *opaque;

    if (!PyArg_ParseTuple
        (args, (char *) "iiiOO:virEventInvokeHandleCallback",
         &watch, &fd, &event, &py_f, &py_opaque
        ))
        return VIR_PY_INT_FAIL;

    cb     = (virEventHandleCallback) PyvirEventHandleCallback_Get(py_f);
    opaque = (void *) PyvirVoidPtr_Get(py_opaque);

    if (cb) {
        LIBVIRT_BEGIN_ALLOW_THREADS;
        cb(watch, fd, event, opaque);
        LIBVIRT_END_ALLOW_THREADS;
    }

    return VIR_PY_INT_SUCCESS;
}

static PyObject *
libvirt_virEventInvokeTimeoutCallback(PyObject *self ATTRIBUTE_UNUSED,
                                      PyObject *args)
{
    int timer;
    PyObject *py_f;
    PyObject *py_opaque;
    virEventTimeoutCallback cb;
    void *opaque;

    if (!PyArg_ParseTuple
        (args, (char *) "iOO:virEventInvokeTimeoutCallback",
                        &timer, &py_f, &py_opaque
        ))
        return VIR_PY_INT_FAIL;

    cb     = (virEventTimeoutCallback) PyvirEventTimeoutCallback_Get(py_f);
    opaque = (void *) PyvirVoidPtr_Get(py_opaque);
    if (cb) {
        LIBVIRT_BEGIN_ALLOW_THREADS;
        cb(timer, opaque);
        LIBVIRT_END_ALLOW_THREADS;
    }

    return VIR_PY_INT_SUCCESS;
}

static void
libvirt_virEventHandleCallback(int watch,
                               int fd,
                               int events,
                               void *opaque)
{
    PyObject *pyobj_cbData = (PyObject *)opaque;
    PyObject *pyobj_ret;
    PyObject *python_cb;

    LIBVIRT_ENSURE_THREAD_STATE;

    /* Lookup the python callback */
    python_cb = libvirt_lookupPythonFunc("_dispatchEventHandleCallback");
    if (!python_cb) {
        goto cleanup;
    }

    Py_INCREF(pyobj_cbData);

    /* Call the pure python dispatcher */
    pyobj_ret = PyObject_CallFunction(python_cb,
                                      (char *)"iiiO",
                                      watch, fd, events, pyobj_cbData);

    Py_DECREF(pyobj_cbData);

    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
    }

cleanup:
    LIBVIRT_RELEASE_THREAD_STATE;
}

static PyObject *
libvirt_virEventAddHandle(PyObject *self ATTRIBUTE_UNUSED,
                          PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_cbData;
    virEventHandleCallback cb = libvirt_virEventHandleCallback;
    int events;
    int fd;
    int ret;

    if (!PyArg_ParseTuple(args, (char *) "iiO:virEventAddHandle",
                          &fd, &events, &pyobj_cbData)) {
        DEBUG("%s failed to parse tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    Py_INCREF(pyobj_cbData);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virEventAddHandle(fd, events, cb, pyobj_cbData, NULL);
    LIBVIRT_END_ALLOW_THREADS;

    if (ret < 0) {
        Py_DECREF(pyobj_cbData);
    }

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static void
libvirt_virEventTimeoutCallback(int timer,
                                void *opaque)
{
    PyObject *pyobj_cbData = (PyObject *)opaque;
    PyObject *pyobj_ret;
    PyObject *python_cb;

    LIBVIRT_ENSURE_THREAD_STATE;

    /* Lookup the python callback */
    python_cb = libvirt_lookupPythonFunc("_dispatchEventTimeoutCallback");
    if (!python_cb) {
        goto cleanup;
    }

    Py_INCREF(pyobj_cbData);

    /* Call the pure python dispatcher */
    pyobj_ret = PyObject_CallFunction(python_cb,
                                      (char *)"iO",
                                      timer, pyobj_cbData);

    Py_DECREF(pyobj_cbData);

    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
    }

cleanup:
    LIBVIRT_RELEASE_THREAD_STATE;
}

static PyObject *
libvirt_virEventAddTimeout(PyObject *self ATTRIBUTE_UNUSED,
                           PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_cbData;
    virEventTimeoutCallback cb = libvirt_virEventTimeoutCallback;
    int timeout;
    int ret;

    if (!PyArg_ParseTuple(args, (char *) "iO:virEventAddTimeout",
                          &timeout, &pyobj_cbData)) {
        DEBUG("%s failed to parse tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    Py_INCREF(pyobj_cbData);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virEventAddTimeout(timeout, cb, pyobj_cbData, NULL);
    LIBVIRT_END_ALLOW_THREADS;

    if (ret < 0) {
        Py_DECREF(pyobj_cbData);
    }

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static void
libvirt_virConnectDomainEventFreeFunc(void *opaque)
{
    PyObject *pyobj_conn = (PyObject*)opaque;
    LIBVIRT_ENSURE_THREAD_STATE;
    Py_DECREF(pyobj_conn);
    LIBVIRT_RELEASE_THREAD_STATE;
}

static int
libvirt_virConnectDomainEventLifecycleCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                               virDomainPtr dom,
                                               int event,
                                               int detail,
                                               void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventLifecycleCallback",
                                    (char*)"OiiO",
                                    pyobj_dom,
                                    event, detail,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventGenericCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                             virDomainPtr dom,
                                             void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventGenericCallback",
                                    (char*)"OO",
                                    pyobj_dom, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventRTCChangeCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                               virDomainPtr dom,
                                               long long utcoffset,
                                               void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventRTCChangeCallback",
                                    (char*)"OLO",
                                    pyobj_dom,
                                    (PY_LONG_LONG)utcoffset,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventWatchdogCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                              virDomainPtr dom,
                                              int action,
                                              void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventWatchdogCallback",
                                    (char*)"OiO",
                                    pyobj_dom,
                                    action,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventIOErrorCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                             virDomainPtr dom,
                                             const char *srcPath,
                                             const char *devAlias,
                                             int action,
                                             void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventIOErrorCallback",
                                    (char*)"OssiO",
                                    pyobj_dom,
                                    srcPath, devAlias, action,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventIOErrorReasonCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                   virDomainPtr dom,
                                                   const char *srcPath,
                                                   const char *devAlias,
                                                   int action,
                                                   const char *reason,
                                                   void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventIOErrorReasonCallback",
                                    (char*)"OssisO",
                                    pyobj_dom,
                                    srcPath, devAlias, action, reason,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventGraphicsCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                              virDomainPtr dom,
                                              int phase,
                                              virDomainEventGraphicsAddressPtr local,
                                              virDomainEventGraphicsAddressPtr remote,
                                              const char *authScheme,
                                              virDomainEventGraphicsSubjectPtr subject,
                                              void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    PyObject *pyobj_local;
    PyObject *pyobj_remote;
    PyObject *pyobj_subject;
    int ret = -1;
    size_t i;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* FIXME This code should check for errors... */
    pyobj_local = PyDict_New();
    PyDict_SetItem(pyobj_local,
                   libvirt_constcharPtrWrap("family"),
                   libvirt_intWrap(local->family));
    PyDict_SetItem(pyobj_local,
                   libvirt_constcharPtrWrap("node"),
                   libvirt_constcharPtrWrap(local->node));
    PyDict_SetItem(pyobj_local,
                   libvirt_constcharPtrWrap("service"),
                   libvirt_constcharPtrWrap(local->service));

    pyobj_remote = PyDict_New();
    PyDict_SetItem(pyobj_remote,
                   libvirt_constcharPtrWrap("family"),
                   libvirt_intWrap(remote->family));
    PyDict_SetItem(pyobj_remote,
                   libvirt_constcharPtrWrap("node"),
                   libvirt_constcharPtrWrap(remote->node));
    PyDict_SetItem(pyobj_remote,
                   libvirt_constcharPtrWrap("service"),
                   libvirt_constcharPtrWrap(remote->service));

    pyobj_subject = PyList_New(subject->nidentity);
    for (i = 0; i < subject->nidentity; i++) {
        PyObject *pair = PyTuple_New(2);
        PyTuple_SetItem(pair, 0, libvirt_constcharPtrWrap(subject->identities[i].type));
        PyTuple_SetItem(pair, 1, libvirt_constcharPtrWrap(subject->identities[i].name));

        PyList_SetItem(pyobj_subject, i, pair);
    }

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventGraphicsCallback",
                                    (char*)"OiOOsOO",
                                    pyobj_dom,
                                    phase, pyobj_local, pyobj_remote,
                                    authScheme, pyobj_subject,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventBlockJobCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                              virDomainPtr dom,
                                              const char *disk,
                                              int type,
                                              int status,
                                              void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventBlockJobCallback",
                                    (char*)"OsiiO",
                                    pyobj_dom, disk, type, status, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventDiskChangeCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                virDomainPtr dom,
                                                const char *oldSrcPath,
                                                const char *newSrcPath,
                                                const char *devAlias,
                                                int reason,
                                                void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventDiskChangeCallback",
                                    (char*)"OsssiO",
                                    pyobj_dom,
                                    oldSrcPath, newSrcPath,
                                    devAlias, reason, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventTrayChangeCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                virDomainPtr dom,
                                                const char *devAlias,
                                                int reason,
                                                void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventTrayChangeCallback",
                                    (char*)"OsiO",
                                    pyobj_dom,
                                    devAlias, reason, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventPMWakeupCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                              virDomainPtr dom,
                                              int reason,
                                              void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventPMWakeupCallback",
                                    (char*)"OiO",
                                    pyobj_dom,
                                    reason,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static int
libvirt_virConnectDomainEventPMSuspendCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                               virDomainPtr dom,
                                               int reason,
                                               void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventPMSuspendCallback",
                                    (char*)"OiO",
                                    pyobj_dom,
                                    reason,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}


#ifdef VIR_DOMAIN_EVENT_ID_BALLOON_CHANGE
static int
libvirt_virConnectDomainEventBalloonChangeCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                   virDomainPtr dom,
                                                   unsigned long long actual,
                                                   void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventBalloonChangeCallback",
                                    (char*)"OLO",
                                    pyobj_dom,
                                    (PY_LONG_LONG)actual,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}
#endif /* VIR_DOMAIN_EVENT_ID_BALLOON_CHANGE */

#ifdef VIR_DOMAIN_EVENT_ID_PMSUSPEND_DISK
static int
libvirt_virConnectDomainEventPMSuspendDiskCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                   virDomainPtr dom,
                                                   int reason,
                                                   void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventPMSuspendDiskCallback",
                                    (char*)"OiO",
                                    pyobj_dom,
                                    reason,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}
#endif /* VIR_DOMAIN_EVENT_ID_PMSUSPEND_DISK */

#ifdef VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED
static int
libvirt_virConnectDomainEventDeviceRemovedCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                   virDomainPtr dom,
                                                   const char *devAlias,
                                                   void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventDeviceRemovedCallback",
                                    (char*)"OsO",
                                    pyobj_dom, devAlias, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}
#endif /* VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED */

#ifdef VIR_DOMAIN_EVENT_ID_TUNABLE
static int
libvirt_virConnectDomainEventTunableCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                             virDomainPtr dom,
                                             virTypedParameterPtr params,
                                             int nparams,
                                             void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    PyObject *pyobj_dict = NULL;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    pyobj_dict = getPyVirTypedParameter(params, nparams);
    if (!pyobj_dict)
        goto cleanup;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventTunableCallback",
                                    (char*)"OOO",
                                    pyobj_dom, pyobj_dict, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
        Py_XDECREF(pyobj_dict);
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;

}
#endif /* VIR_DOMAIN_EVENT_ID_TUNABLE */

#ifdef VIR_DOMAIN_EVENT_ID_AGENT_LIFECYCLE
static int
libvirt_virConnectDomainEventAgentLifecycleCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                    virDomainPtr dom,
                                                    int state,
                                                    int reason,
                                                    void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventAgentLifecycleCallback",
                                    (char*)"OiiO",
                                    pyobj_dom, state, reason, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;

}
#endif /* VIR_DOMAIN_EVENT_ID_AGENT_LIFECYCLE */

#ifdef VIR_DOMAIN_EVENT_ID_DEVICE_ADDED
static int
libvirt_virConnectDomainEventDeviceAddedCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                 virDomainPtr dom,
                                                 const char *devAlias,
                                                 void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_dom;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virDomainPtr */
    virDomainRef(dom);
    if (!(pyobj_dom = libvirt_virDomainPtrWrap(dom))) {
        virDomainFree(dom);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchDomainEventDeviceAddedCallback",
                                    (char*)"OsO",
                                    pyobj_dom, devAlias, pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_dom);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;

}
#endif /* VIR_DOMAIN_EVENT_ID_DEVICE_ADDED */

static PyObject *
libvirt_virConnectDomainEventRegisterAny(ATTRIBUTE_UNUSED PyObject *self,
                                         PyObject *args)
{
    PyObject *py_retval;        /* return value */
    PyObject *pyobj_conn;       /* virConnectPtr */
    PyObject *pyobj_dom;
    PyObject *pyobj_cbData;     /* hash of callback data */
    int eventID;
    virConnectPtr conn;
    int ret = 0;
    virConnectDomainEventGenericCallback cb = NULL;
    virDomainPtr dom;

    if (!PyArg_ParseTuple
        (args, (char *) "OOiO:virConnectDomainEventRegisterAny",
         &pyobj_conn, &pyobj_dom, &eventID, &pyobj_cbData)) {
        DEBUG("%s failed parsing tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    DEBUG("libvirt_virConnectDomainEventRegister(%p %p %d %p) called\n",
           pyobj_conn, pyobj_dom, eventID, pyobj_cbData);
    conn = PyvirConnect_Get(pyobj_conn);
    if (pyobj_dom == Py_None)
        dom = NULL;
    else
        dom = PyvirDomain_Get(pyobj_dom);

    switch ((virDomainEventID) eventID) {
    case VIR_DOMAIN_EVENT_ID_LIFECYCLE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventLifecycleCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_REBOOT:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventGenericCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_RTC_CHANGE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventRTCChangeCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_WATCHDOG:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventWatchdogCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_IO_ERROR:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventIOErrorCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_IO_ERROR_REASON:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventIOErrorReasonCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_GRAPHICS:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventGraphicsCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_CONTROL_ERROR:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventGenericCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_BLOCK_JOB:
#ifdef VIR_DOMAIN_EVENT_ID_BLOCK_JOB_2
    case VIR_DOMAIN_EVENT_ID_BLOCK_JOB_2:
#endif /* VIR_DOMAIN_EVENT_ID_BLOCK_JOB_2 */
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventBlockJobCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_DISK_CHANGE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventDiskChangeCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_TRAY_CHANGE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventTrayChangeCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_PMWAKEUP:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventPMWakeupCallback);
        break;
    case VIR_DOMAIN_EVENT_ID_PMSUSPEND:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventPMSuspendCallback);
        break;
#ifdef VIR_DOMAIN_EVENT_ID_BALLOON_CHANGE
    case VIR_DOMAIN_EVENT_ID_BALLOON_CHANGE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventBalloonChangeCallback);
        break;
#endif /* VIR_DOMAIN_EVENT_ID_BALLOON_CHANGE */
#ifdef VIR_DOMAIN_EVENT_ID_PMSUSPEND_DISK
    case VIR_DOMAIN_EVENT_ID_PMSUSPEND_DISK:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventPMSuspendDiskCallback);
        break;
#endif /* VIR_DOMAIN_EVENT_ID_PMSUSPEND_DISK */
#ifdef VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED
    case VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventDeviceRemovedCallback);
        break;
#endif /* VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED */
#ifdef VIR_DOMAIN_EVENT_ID_TUNABLE
    case VIR_DOMAIN_EVENT_ID_TUNABLE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventTunableCallback);
        break;
#endif /* VIR_DOMAIN_EVENT_ID_TUNABLE */
#ifdef VIR_DOMAIN_EVENT_ID_AGENT_LIFECYCLE
    case VIR_DOMAIN_EVENT_ID_AGENT_LIFECYCLE:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventAgentLifecycleCallback);
        break;
#endif /* VIR_DOMAIN_EVENT_ID_AGENT_LIFECYCLE */
#ifdef VIR_DOMAIN_EVENT_ID_DEVICE_ADDED
    case VIR_DOMAIN_EVENT_ID_DEVICE_ADDED:
        cb = VIR_DOMAIN_EVENT_CALLBACK(libvirt_virConnectDomainEventDeviceAddedCallback);
        break;
#endif /* VIR_DOMAIN_EVENT_ID_DEVICE_ADDED */
    case VIR_DOMAIN_EVENT_ID_LAST:
        break;
    }

    if (!cb) {
        return VIR_PY_INT_FAIL;
    }

    Py_INCREF(pyobj_cbData);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virConnectDomainEventRegisterAny(conn, dom, eventID,
                                           cb, pyobj_cbData,
                                           libvirt_virConnectDomainEventFreeFunc);
    LIBVIRT_END_ALLOW_THREADS;

    if (ret < 0) {
        Py_DECREF(pyobj_cbData);
    }

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static PyObject *
libvirt_virConnectDomainEventDeregisterAny(ATTRIBUTE_UNUSED PyObject *self,
                                           PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_conn;
    int callbackID;
    virConnectPtr conn;
    int ret = 0;

    if (!PyArg_ParseTuple
        (args, (char *) "Oi:virConnectDomainEventDeregister",
         &pyobj_conn, &callbackID))
        return NULL;

    DEBUG("libvirt_virConnectDomainEventDeregister(%p) called\n", pyobj_conn);

    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    ret = virConnectDomainEventDeregisterAny(conn, callbackID);

    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(1, 2, 1)
static void
libvirt_virConnectNetworkEventFreeFunc(void *opaque)
{
    PyObject *pyobj_conn = (PyObject*)opaque;
    LIBVIRT_ENSURE_THREAD_STATE;
    Py_DECREF(pyobj_conn);
    LIBVIRT_RELEASE_THREAD_STATE;
}

static int
libvirt_virConnectNetworkEventLifecycleCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                                                virNetworkPtr net,
                                                int event,
                                                int detail,
                                                void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_net;
    PyObject *pyobj_ret = NULL;
    PyObject *pyobj_conn;
    PyObject *dictKey;
    int ret = -1;

    LIBVIRT_ENSURE_THREAD_STATE;

    if (!(dictKey = libvirt_constcharPtrWrap("conn")))
        goto cleanup;
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Create a python instance of this virNetworkPtr */
    virNetworkRef(net);
    if (!(pyobj_net = libvirt_virNetworkPtrWrap(net))) {
        virNetworkFree(net);
        goto cleanup;
    }
    Py_INCREF(pyobj_cbData);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchNetworkEventLifecycleCallback",
                                    (char*)"OiiO",
                                    pyobj_net,
                                    event,
                                    detail,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);
    Py_DECREF(pyobj_net);

 cleanup:
    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
        ret = 0;
    }

    LIBVIRT_RELEASE_THREAD_STATE;
    return ret;
}

static PyObject *
libvirt_virConnectNetworkEventRegisterAny(PyObject *self ATTRIBUTE_UNUSED,
                                          PyObject *args)
{
    PyObject *py_retval;        /* return value */
    PyObject *pyobj_conn;       /* virConnectPtr */
    PyObject *pyobj_net;
    PyObject *pyobj_cbData;     /* hash of callback data */
    int eventID;
    virConnectPtr conn;
    int ret = 0;
    virConnectNetworkEventGenericCallback cb = NULL;
    virNetworkPtr net;

    if (!PyArg_ParseTuple
        (args, (char *) "OOiO:virConnectNetworkEventRegisterAny",
         &pyobj_conn, &pyobj_net, &eventID, &pyobj_cbData)) {
        DEBUG("%s failed parsing tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    DEBUG("libvirt_virConnectNetworkEventRegister(%p %p %d %p) called\n",
           pyobj_conn, pyobj_net, eventID, pyobj_cbData);
    conn = PyvirConnect_Get(pyobj_conn);
    if (pyobj_net == Py_None)
        net = NULL;
    else
        net = PyvirNetwork_Get(pyobj_net);

    switch ((virNetworkEventID) eventID) {
    case VIR_NETWORK_EVENT_ID_LIFECYCLE:
        cb = VIR_NETWORK_EVENT_CALLBACK(libvirt_virConnectNetworkEventLifecycleCallback);
        break;

    case VIR_NETWORK_EVENT_ID_LAST:
        break;
    }

    if (!cb) {
        return VIR_PY_INT_FAIL;
    }

    Py_INCREF(pyobj_cbData);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virConnectNetworkEventRegisterAny(conn, net, eventID,
                                            cb, pyobj_cbData,
                                            libvirt_virConnectNetworkEventFreeFunc);
    LIBVIRT_END_ALLOW_THREADS;

    if (ret < 0) {
        Py_DECREF(pyobj_cbData);
    }

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static PyObject
*libvirt_virConnectNetworkEventDeregisterAny(PyObject *self ATTRIBUTE_UNUSED,
                                             PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_conn;
    int callbackID;
    virConnectPtr conn;
    int ret = 0;

    if (!PyArg_ParseTuple
        (args, (char *) "Oi:virConnectNetworkEventDeregister",
         &pyobj_conn, &callbackID))
        return NULL;

    DEBUG("libvirt_virConnectNetworkEventDeregister(%p) called\n", pyobj_conn);

    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    ret = virConnectNetworkEventDeregisterAny(conn, callbackID);

    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_intWrap(ret);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(1, 2, 1)*/

#if LIBVIR_CHECK_VERSION(0, 10, 0)
static void
libvirt_virConnectCloseCallbackDispatch(virConnectPtr conn ATTRIBUTE_UNUSED,
                                        int reason,
                                        void *opaque)
{
    PyObject *pyobj_cbData = (PyObject*)opaque;
    PyObject *pyobj_ret;
    PyObject *pyobj_conn;
    PyObject *dictKey;

    LIBVIRT_ENSURE_THREAD_STATE;

    Py_INCREF(pyobj_cbData);

    dictKey = libvirt_constcharPtrWrap("conn");
    pyobj_conn = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Call the Callback Dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_conn,
                                    (char*)"_dispatchCloseCallback",
                                    (char*)"iO",
                                    reason,
                                    pyobj_cbData);

    Py_DECREF(pyobj_cbData);

    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
    }

    LIBVIRT_RELEASE_THREAD_STATE;
}

static PyObject *
libvirt_virConnectRegisterCloseCallback(ATTRIBUTE_UNUSED PyObject * self,
                                        PyObject * args)
{
    PyObject *py_retval;        /* return value */
    PyObject *pyobj_conn;       /* virConnectPtr */
    PyObject *pyobj_cbData;     /* hash of callback data */
    virConnectPtr conn;
    int ret = 0;

    if (!PyArg_ParseTuple
        (args, (char *) "OO:virConnectRegisterCloseCallback",
         &pyobj_conn, &pyobj_cbData)) {
        DEBUG("%s failed parsing tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    DEBUG("libvirt_virConnectRegisterCloseCallback(%p %p) called\n",
           pyobj_conn, pyobj_cbData);
    conn = PyvirConnect_Get(pyobj_conn);

    Py_INCREF(pyobj_cbData);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virConnectRegisterCloseCallback(conn,
                                          libvirt_virConnectCloseCallbackDispatch,
                                          pyobj_cbData,
                                          libvirt_virConnectDomainEventFreeFunc);
    LIBVIRT_END_ALLOW_THREADS;

    if (ret < 0) {
        Py_DECREF(pyobj_cbData);
    }

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static PyObject *
libvirt_virConnectUnregisterCloseCallback(ATTRIBUTE_UNUSED PyObject * self,
                                          PyObject * args)
{
    PyObject *py_retval;
    PyObject *pyobj_conn;
    virConnectPtr conn;
    int ret = 0;

    if (!PyArg_ParseTuple
        (args, (char *) "O:virConnectDomainEventUnregister",
         &pyobj_conn))
        return NULL;

    DEBUG("libvirt_virConnectDomainEventUnregister(%p) called\n",
          pyobj_conn);

    conn = PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;

    ret = virConnectUnregisterCloseCallback(conn,
                                            libvirt_virConnectCloseCallbackDispatch);

    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_intWrap(ret);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 0) */

static void
libvirt_virStreamEventFreeFunc(void *opaque)
{
    PyObject *pyobj_stream = (PyObject*)opaque;
    LIBVIRT_ENSURE_THREAD_STATE;
    Py_DECREF(pyobj_stream);
    LIBVIRT_RELEASE_THREAD_STATE;
}

static void
libvirt_virStreamEventCallback(virStreamPtr st ATTRIBUTE_UNUSED,
                               int events,
                               void *opaque)
{
    PyObject *pyobj_cbData = (PyObject *)opaque;
    PyObject *pyobj_stream;
    PyObject *pyobj_ret;
    PyObject *dictKey;

    LIBVIRT_ENSURE_THREAD_STATE;

    Py_INCREF(pyobj_cbData);
    dictKey = libvirt_constcharPtrWrap("stream");
    pyobj_stream = PyDict_GetItem(pyobj_cbData, dictKey);
    Py_DECREF(dictKey);

    /* Call the pure python dispatcher */
    pyobj_ret = PyObject_CallMethod(pyobj_stream,
                                    (char *)"_dispatchStreamEventCallback",
                                    (char *)"iO",
                                    events, pyobj_cbData);

    Py_DECREF(pyobj_cbData);

    if (!pyobj_ret) {
        DEBUG("%s - ret:%p\n", __FUNCTION__, pyobj_ret);
        PyErr_Print();
    } else {
        Py_DECREF(pyobj_ret);
    }

    LIBVIRT_RELEASE_THREAD_STATE;
}

static PyObject *
libvirt_virStreamEventAddCallback(PyObject *self ATTRIBUTE_UNUSED,
                                  PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_stream;
    PyObject *pyobj_cbData;
    virStreamPtr stream;
    virStreamEventCallback cb = libvirt_virStreamEventCallback;
    int ret;
    int events;

    if (!PyArg_ParseTuple(args, (char *) "OiO:virStreamEventAddCallback",
                          &pyobj_stream, &events, &pyobj_cbData)) {
        DEBUG("%s failed to parse tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }

    DEBUG("libvirt_virStreamEventAddCallback(%p, %d, %p) called\n",
          pyobj_stream, events, pyobj_cbData);
    stream = PyvirStream_Get(pyobj_stream);

    Py_INCREF(pyobj_cbData);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virStreamEventAddCallback(stream, events, cb, pyobj_cbData,
                                    libvirt_virStreamEventFreeFunc);
    LIBVIRT_END_ALLOW_THREADS;

    if (ret < 0) {
        Py_DECREF(pyobj_cbData);
    }

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static PyObject *
libvirt_virStreamRecv(PyObject *self ATTRIBUTE_UNUSED,
                      PyObject *args)
{
    PyObject *pyobj_stream;
    PyObject *rv;
    virStreamPtr stream;
    char *buf = NULL;
    int ret;
    int nbytes;

    if (!PyArg_ParseTuple(args, (char *) "Oi:virStreamRecv",
                          &pyobj_stream, &nbytes)) {
        DEBUG("%s failed to parse tuple\n", __FUNCTION__);
        return VIR_PY_NONE;
    }
    stream = PyvirStream_Get(pyobj_stream);

    if (VIR_ALLOC_N(buf, nbytes+1 > 0 ? nbytes+1 : 1) < 0)
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virStreamRecv(stream, buf, nbytes);
    LIBVIRT_END_ALLOW_THREADS;

    buf[ret > -1 ? ret : 0] = '\0';
    DEBUG("StreamRecv ret=%d strlen=%d\n", ret, (int) strlen(buf));

    if (ret == -2)
        return libvirt_intWrap(ret);
    if (ret < 0)
        return VIR_PY_NONE;
    rv = libvirt_charPtrSizeWrap((char *) buf, (Py_ssize_t) ret);
    VIR_FREE(buf);
    return rv;
}

static PyObject *
libvirt_virStreamSend(PyObject *self ATTRIBUTE_UNUSED,
                      PyObject *args)
{
    PyObject *py_retval;
    PyObject *pyobj_stream;
    PyObject *pyobj_data;
    virStreamPtr stream;
    char *data;
    Py_ssize_t datalen;
    int ret;

    if (!PyArg_ParseTuple(args, (char *) "OO:virStreamRecv",
                          &pyobj_stream, &pyobj_data)) {
        DEBUG("%s failed to parse tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }
    stream = PyvirStream_Get(pyobj_stream);
    libvirt_charPtrSizeUnwrap(pyobj_data, &data, &datalen);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virStreamSend(stream, data, datalen);
    LIBVIRT_END_ALLOW_THREADS;

    DEBUG("StreamSend ret=%d\n", ret);

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

static PyObject *
libvirt_virDomainSendKey(PyObject *self ATTRIBUTE_UNUSED,
                         PyObject *args)
{
    PyObject *py_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *pyobj_list;
    int codeset;
    int holdtime;
    unsigned int flags;
    int ret;
    size_t i;
    unsigned int keycodes[VIR_DOMAIN_SEND_KEY_MAX_KEYS];
    unsigned int nkeycodes;

    if (!PyArg_ParseTuple(args, (char *)"OiiOiI:virDomainSendKey",
                          &pyobj_domain, &codeset, &holdtime, &pyobj_list,
                          &nkeycodes, &flags)) {
        DEBUG("%s failed to parse tuple\n", __FUNCTION__);
        return VIR_PY_INT_FAIL;
    }
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (!PyList_Check(pyobj_list)) {
        return VIR_PY_INT_FAIL;
    }

    if (nkeycodes != PyList_Size(pyobj_list) ||
        nkeycodes > VIR_DOMAIN_SEND_KEY_MAX_KEYS) {
        return VIR_PY_INT_FAIL;
    }

    for (i = 0; i < nkeycodes; i++) {
        if (libvirt_uintUnwrap(PyList_GetItem(pyobj_list, i), &keycodes[i]) < 0)
            return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virDomainSendKey(domain, codeset, holdtime, keycodes, nkeycodes, flags);
    LIBVIRT_END_ALLOW_THREADS;

    DEBUG("virDomainSendKey ret=%d\n", ret);

    py_retval = libvirt_intWrap(ret);
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(1, 0, 3)
static PyObject *
libvirt_virDomainMigrateGetCompressionCache(PyObject *self ATTRIBUTE_UNUSED,
                                            PyObject *args)
{
    PyObject *pyobj_domain;
    virDomainPtr domain;
    unsigned int flags;
    unsigned long long cacheSize;
    int rc;

    if (!PyArg_ParseTuple(args,
                          (char *) "OI:virDomainMigrateGetCompressionCache",
                          &pyobj_domain, &flags))
        return VIR_PY_NONE;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    rc = virDomainMigrateGetCompressionCache(domain, &cacheSize, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (rc < 0)
        return VIR_PY_NONE;

    return libvirt_ulonglongWrap(cacheSize);
}
#endif /* LIBVIR_CHECK_VERSION(1, 0, 3) */

static PyObject *
libvirt_virDomainMigrateGetMaxSpeed(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval;
    int c_retval;
    unsigned long bandwidth;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    unsigned int flags = 0;

    if (!PyArg_ParseTuple(args, (char *)"OI:virDomainMigrateGetMaxSpeed",
                          &pyobj_domain, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainMigrateGetMaxSpeed(domain, &bandwidth, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        return VIR_PY_INT_FAIL;
    py_retval = libvirt_ulongWrap(bandwidth);
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(1, 1, 0)
static PyObject *
libvirt_virDomainMigrate3(PyObject *self ATTRIBUTE_UNUSED,
                          PyObject *args)
{
    PyObject *pyobj_domain;
    virDomainPtr domain;
    PyObject *pyobj_dconn;
    virConnectPtr dconn;
    PyObject *dict;
    unsigned int flags;
    virTypedParameterPtr params;
    int nparams;
    virDomainPtr ddom = NULL;

    if (!PyArg_ParseTuple(args, (char *) "OOOI:virDomainMigrate3",
                          &pyobj_domain, &pyobj_dconn, &dict, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);
    dconn = (virConnectPtr) PyvirConnect_Get(pyobj_dconn);

    if (virPyDictToTypedParams(dict, &params, &nparams, NULL, 0) < 0)
        return NULL;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ddom = virDomainMigrate3(domain, dconn, params, nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    virTypedParamsFree(params, nparams);
    return libvirt_virDomainPtrWrap(ddom);
}

static PyObject *
libvirt_virDomainMigrateToURI3(PyObject *self ATTRIBUTE_UNUSED,
                               PyObject *args)
{
    PyObject *pyobj_domain;
    virDomainPtr domain;
    char *dconnuri;
    PyObject *dict;
    unsigned int flags;
    virTypedParameterPtr params;
    int nparams;
    int ret = -1;

    if (!PyArg_ParseTuple(args, (char *) "OzOI:virDomainMigrate3",
                          &pyobj_domain, &dconnuri, &dict, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (virPyDictToTypedParams(dict, &params, &nparams, NULL, 0) < 0)
        return NULL;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    ret = virDomainMigrateToURI3(domain, dconnuri, params, nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    virTypedParamsFree(params, nparams);
    return libvirt_intWrap(ret);
}
#endif /* LIBVIR_CHECK_VERSION(1, 1, 0) */

static PyObject *
libvirt_virDomainBlockPeek(PyObject *self ATTRIBUTE_UNUSED,
                           PyObject *args) {
    PyObject *py_retval = NULL;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    const char *disk;
    unsigned long long offset;
    size_t size;
    char *buf;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OzLnI:virDomainBlockPeek", &pyobj_domain,
                          &disk, &offset, &size, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (VIR_ALLOC_N(buf, size) < 0)
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainBlockPeek(domain, disk, offset, size, buf, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    py_retval = libvirt_charPtrSizeWrap(buf, size);

cleanup:
    VIR_FREE(buf);
    return py_retval;
}

static PyObject *
libvirt_virDomainMemoryPeek(PyObject *self ATTRIBUTE_UNUSED,
                            PyObject *args) {
    PyObject *py_retval = NULL;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    unsigned long long start;
    size_t size;
    char *buf;
    unsigned int flags;

    if (!PyArg_ParseTuple(args, (char *)"OLnI:virDomainMemoryPeek", &pyobj_domain,
                          &start, &size, &flags))
        return NULL;

    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (VIR_ALLOC_N(buf, size) < 0)
        return VIR_PY_NONE;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainMemoryPeek(domain, start, size, buf, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    py_retval = libvirt_charPtrSizeWrap(buf, size);

cleanup:
    VIR_FREE(buf);
    return py_retval;
}

#if LIBVIR_CHECK_VERSION(0, 10, 2)
static PyObject *
libvirt_virNodeSetMemoryParameters(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    virConnectPtr conn;
    PyObject *pyobj_conn, *info;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    Py_ssize_t size = 0;
    unsigned int flags;
    virTypedParameterPtr params, new_params = NULL;

    if (!PyArg_ParseTuple(args,
                          (char *)"OOI:virNodeSetMemoryParameters",
                          &pyobj_conn, &info, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((size = PyDict_Size(info)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to set attributes");
        return NULL;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeGetMemoryParameters(conn, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_INT_FAIL;

    if (nparams == 0) {
        PyErr_Format(PyExc_LookupError,
                     "no settable attributes");
        return NULL;
    }

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeGetMemoryParameters(conn, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    new_params = setPyVirTypedParameter(info, params, nparams);
    if (!new_params)
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeSetMemoryParameters(conn, new_params, size, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_INT_FAIL;
        goto cleanup;
    }

    ret = VIR_PY_INT_SUCCESS;

cleanup:
    virTypedParamsFree(params, nparams);
    virTypedParamsFree(new_params, nparams);
    return ret;
}

static PyObject *
libvirt_virNodeGetMemoryParameters(PyObject *self ATTRIBUTE_UNUSED,
                                   PyObject *args)
{
    virConnectPtr conn;
    PyObject *pyobj_conn;
    PyObject *ret = NULL;
    int i_retval;
    int nparams = 0;
    unsigned int flags;
    virTypedParameterPtr params;

    if (!PyArg_ParseTuple(args, (char *)"OI:virNodeGetMemoryParameters",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeGetMemoryParameters(conn, NULL, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if (!nparams)
        return PyDict_New();

    if (VIR_ALLOC_N(params, nparams) < 0)
        return PyErr_NoMemory();

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeGetMemoryParameters(conn, params, &nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0) {
        ret = VIR_PY_NONE;
        goto cleanup;
    }

    ret = getPyVirTypedParameter(params, nparams);

cleanup:
    virTypedParamsFree(params, nparams);
    return ret;
}
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */

#if LIBVIR_CHECK_VERSION(1, 0, 0)
static PyObject *
libvirt_virNodeGetCPUMap(PyObject *self ATTRIBUTE_UNUSED,
                         PyObject *args)
{
    virConnectPtr conn;
    PyObject *pyobj_conn;
    PyObject *ret = NULL;
    PyObject *pycpumap = NULL;
    PyObject *pyused = NULL;
    PyObject *pycpunum = NULL;
    PyObject *pyonline = NULL;
    int i_retval;
    unsigned char *cpumap = NULL;
    unsigned int online = 0;
    unsigned int flags;
    size_t i;

    if (!PyArg_ParseTuple(args, (char *)"OI:virNodeGetCPUMap",
                          &pyobj_conn, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    i_retval = virNodeGetCPUMap(conn, &cpumap, &online, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (i_retval < 0)
        return VIR_PY_NONE;

    if ((ret = PyTuple_New(3)) == NULL)
        goto error;

    /* 0: number of CPUs */
    if ((pycpunum = libvirt_intWrap(i_retval)) == NULL ||
        PyTuple_SetItem(ret, 0, pycpunum) < 0)
        goto error;

    /* 1: CPU map */
    if ((pycpumap = PyList_New(i_retval)) == NULL)
        goto error;

    for (i = 0; i < i_retval; i++) {
        if ((pyused = PyBool_FromLong(VIR_CPU_USED(cpumap, i))) == NULL)
            goto error;
        if (PyList_SetItem(pycpumap, i, pyused) < 0)
            goto error;
    }

    if (PyTuple_SetItem(ret, 1, pycpumap) < 0)
        goto error;

    /* 2: number of online CPUs */
    if ((pyonline = libvirt_uintWrap(online)) == NULL ||
        PyTuple_SetItem(ret, 2, pyonline) < 0)
        goto error;

cleanup:
    VIR_FREE(cpumap);
    return ret;
error:
    Py_XDECREF(ret);
    Py_XDECREF(pycpumap);
    Py_XDECREF(pyused);
    Py_XDECREF(pycpunum);
    Py_XDECREF(pyonline);
    ret = NULL;
    goto cleanup;
}
#endif /* LIBVIR_CHECK_VERSION(1, 0, 0) */


#if LIBVIR_CHECK_VERSION(1, 1, 1)
static PyObject *
libvirt_virDomainCreateWithFiles(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval = NULL;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *pyobj_files;
    unsigned int flags;
    unsigned int nfiles;
    int *files = NULL;
    size_t i;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virDomainCreateWithFiles",
                          &pyobj_domain, &pyobj_files, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    nfiles = PyList_Size(pyobj_files);

    if (VIR_ALLOC_N(files, nfiles) < 0)
        return PyErr_NoMemory();

    for (i = 0; i < nfiles; i++) {
        PyObject *pyfd;
        int fd;

        pyfd = PyList_GetItem(pyobj_files, i);

        if (libvirt_intUnwrap(pyfd, &fd) < 0)
            goto cleanup;

        files[i] = fd;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainCreateWithFiles(domain, nfiles, files, flags);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_intWrap((int) c_retval);

cleanup:
    VIR_FREE(files);
    return py_retval;
}


static PyObject *
libvirt_virDomainCreateXMLWithFiles(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval = NULL;
    virDomainPtr c_retval;
    virConnectPtr conn;
    PyObject *pyobj_conn;
    char * xmlDesc;
    PyObject *pyobj_files;
    unsigned int flags;
    unsigned int nfiles;
    int *files = NULL;
    size_t i;

    if (!PyArg_ParseTuple(args, (char *)"OzOI:virDomainCreateXMLWithFiles",
                          &pyobj_conn, &xmlDesc, &pyobj_files, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    nfiles = PyList_Size(pyobj_files);

    if (VIR_ALLOC_N(files, nfiles) < 0)
        return PyErr_NoMemory();

    for (i = 0; i < nfiles; i++) {
        PyObject *pyfd;
        int fd;

        pyfd = PyList_GetItem(pyobj_files, i);

        if (libvirt_intUnwrap(pyfd, &fd) < 0)
            goto cleanup;

        files[i] = fd;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainCreateXMLWithFiles(conn, xmlDesc, nfiles, files, flags);
    LIBVIRT_END_ALLOW_THREADS;
    py_retval = libvirt_virDomainPtrWrap((virDomainPtr) c_retval);

cleanup:
    VIR_FREE(files);
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(1, 1, 1) */


#if LIBVIR_CHECK_VERSION(1, 2, 5)
static PyObject *
libvirt_virDomainFSFreeze(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval = NULL;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *pyobj_list;
    unsigned int flags;
    unsigned int nmountpoints = 0;
    char **mountpoints = NULL;
    size_t i = 0, j;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virDomainFSFreeze",
                          &pyobj_domain, &pyobj_list, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (PyList_Check(pyobj_list)) {
        nmountpoints = PyList_Size(pyobj_list);

        if (VIR_ALLOC_N(mountpoints, nmountpoints) < 0)
            return PyErr_NoMemory();

        for (i = 0; i < nmountpoints; i++) {
            if (libvirt_charPtrUnwrap(PyList_GetItem(pyobj_list, i),
                                      mountpoints+i) < 0 ||
                mountpoints[i] == NULL)
                goto cleanup;
        }
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainFSFreeze(domain, (const char **) mountpoints,
                                 nmountpoints, flags);
    LIBVIRT_END_ALLOW_THREADS;

    py_retval = libvirt_intWrap(c_retval);

cleanup:
    for (j = 0 ; j < i ; j++)
        VIR_FREE(mountpoints[j]);
    VIR_FREE(mountpoints);
    return py_retval;
}


static PyObject *
libvirt_virDomainFSThaw(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval = NULL;
    int c_retval;
    virDomainPtr domain;
    PyObject *pyobj_domain;
    PyObject *pyobj_list;
    unsigned int flags;
    unsigned int nmountpoints = 0;
    char **mountpoints = NULL;
    size_t i = 0, j;

    if (!PyArg_ParseTuple(args, (char *)"OOI:virDomainFSThaw",
                          &pyobj_domain, &pyobj_list, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (PyList_Check(pyobj_list)) {
        nmountpoints = PyList_Size(pyobj_list);

        if (VIR_ALLOC_N(mountpoints, nmountpoints) < 0)
            return PyErr_NoMemory();

        for (i = 0; i < nmountpoints; i++) {
            if (libvirt_charPtrUnwrap(PyList_GetItem(pyobj_list, i),
                                      mountpoints+i) < 0 ||
                mountpoints[i] == NULL)
                goto cleanup;
        }
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainFSThaw(domain, (const char **) mountpoints,
                               nmountpoints, flags);
    LIBVIRT_END_ALLOW_THREADS;

    py_retval = libvirt_intWrap(c_retval);

 cleanup:
    for (j = 0 ; j < i ; j++)
        VIR_FREE(mountpoints[j]);
    VIR_FREE(mountpoints);
    return py_retval;
}

static PyObject *
libvirt_virDomainGetTime(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval = NULL;
    PyObject *dict = NULL;
    PyObject *pyobj_domain, *pyobj_seconds, *pyobj_nseconds;
    virDomainPtr domain;
    long long seconds;
    unsigned int nseconds;
    unsigned int flags;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char*)"OI:virDomainGetTime",
                          &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (!(dict = PyDict_New()))
        goto cleanup;

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetTime(domain, &seconds, &nseconds, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    if (!(pyobj_seconds = libvirt_longlongWrap(seconds)) ||
        PyDict_SetItemString(dict, "seconds", pyobj_seconds) < 0)
        goto cleanup;
    Py_DECREF(pyobj_seconds);

    if (!(pyobj_nseconds = libvirt_uintWrap(nseconds)) ||
        PyDict_SetItemString(dict, "nseconds", pyobj_nseconds) < 0)
        goto cleanup;
    Py_DECREF(pyobj_nseconds);

    py_retval = dict;
    dict = NULL;
 cleanup:
    Py_XDECREF(dict);
    return py_retval;
}


static PyObject *
libvirt_virDomainSetTime(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    PyObject *py_retval = NULL;
    PyObject *pyobj_domain;
    PyObject *pyobj_seconds;
    PyObject *pyobj_nseconds;
    PyObject *py_dict;
    virDomainPtr domain;
    long long seconds = 0;
    unsigned int nseconds = 0;
    unsigned int flags;
    ssize_t py_dict_size = 0;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char*)"OOI:virDomainSetTime",
                          &pyobj_domain, &py_dict, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    if (PyDict_Check(py_dict)) {
        py_dict_size = PyDict_Size(py_dict);
        if ((pyobj_seconds = PyDict_GetItemString(py_dict, "seconds"))) {
            if (libvirt_longlongUnwrap(pyobj_seconds, &seconds) < 0) {
                PyErr_Format(PyExc_LookupError, "malformed 'seconds'");
                goto cleanup;
            }
        } else {
            PyErr_Format(PyExc_LookupError, "Dictionary must contains 'seconds'");
            goto cleanup;
        }

        if ((pyobj_nseconds = PyDict_GetItemString(py_dict, "nseconds"))) {
            if (libvirt_uintUnwrap(pyobj_nseconds, &nseconds) < 0) {
                PyErr_Format(PyExc_LookupError, "malformed 'nseconds'");
                goto cleanup;
            }
        } else if (py_dict_size > 1) {
            PyErr_Format(PyExc_LookupError, "Dictionary contains unknown key");
            goto cleanup;
        }
    } else if (py_dict != Py_None || !flags) {
        PyErr_Format(PyExc_TypeError, "time must be a dictionary "
                     "or None with flags set");
        goto cleanup;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainSetTime(domain, seconds, nseconds, flags);
    LIBVIRT_END_ALLOW_THREADS;

    py_retval = libvirt_intWrap(c_retval);

 cleanup:
    return py_retval;
}
#endif /* LIBVIR_CHECK_VERSION(1, 2, 5) */


#if LIBVIR_CHECK_VERSION(1, 2, 6)
static PyObject *
libvirt_virNodeGetFreePages(PyObject *self ATTRIBUTE_UNUSED,
                            PyObject *args) {
    PyObject *py_retval = NULL;
    PyObject *pyobj_conn;
    PyObject *pyobj_pagesize;
    PyObject *pyobj_counts = NULL;
    virConnectPtr conn;
    unsigned int *pages = NULL;
    int startCell;
    unsigned int cellCount;
    unsigned int flags;
    unsigned long long *counts = NULL;
    int c_retval;
    ssize_t pyobj_pagesize_size, i, j;

    if (!PyArg_ParseTuple(args, (char *)"OOiiI:virNodeGetFreePages",
                          &pyobj_conn, &pyobj_pagesize, &startCell,
                          &cellCount, &flags))
        return NULL;

    if (!PyList_Check(pyobj_pagesize)) {
        PyErr_Format(PyExc_TypeError, "pagesize must be list");
        return NULL;
    }

    if (cellCount == 0) {
        PyErr_Format(PyExc_LookupError, "cellCount must not be zero");
        return NULL;
    }

    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    pyobj_pagesize_size = PyList_Size(pyobj_pagesize);
    if (VIR_ALLOC_N(pages, pyobj_pagesize_size) < 0 ||
        VIR_ALLOC_N(counts, pyobj_pagesize_size * cellCount) < 0 ||
        !(pyobj_counts = PyDict_New()))
        goto cleanup;

    for (i = 0; i < pyobj_pagesize_size; i++) {
        PyObject *tmp = PyList_GetItem(pyobj_pagesize, i);

        if (libvirt_uintUnwrap(tmp, &pages[i]) < 0)
            goto cleanup;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeGetFreePages(conn,
                                   pyobj_pagesize_size, pages,
                                   startCell, cellCount,
                                   counts, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    for (i = 0; i < c_retval;) {
        PyObject *per_node = NULL;
        PyObject *node = NULL;

        if (!(per_node = PyDict_New()) ||
            !(node = libvirt_intWrap(startCell + i/pyobj_pagesize_size))) {
            Py_XDECREF(per_node);
            Py_XDECREF(node);
            goto cleanup;
        }

        for (j = 0; j < pyobj_pagesize_size; j ++) {
            PyObject *page = NULL;
            PyObject *count = NULL;

            if (!(page = libvirt_intWrap(pages[j])) ||
                !(count = libvirt_intWrap(counts[i + j])) ||
                PyDict_SetItem(per_node, page, count) < 0) {
                Py_XDECREF(page);
                Py_XDECREF(count);
                Py_XDECREF(per_node);
                Py_XDECREF(node);
                goto cleanup;
            }
        }
        i += pyobj_pagesize_size;

        if (PyDict_SetItem(pyobj_counts, node, per_node) < 0) {
            Py_XDECREF(per_node);
            Py_XDECREF(node);
            goto cleanup;
        }
    }

    py_retval = pyobj_counts;
    pyobj_counts = NULL;
 cleanup:
    Py_XDECREF(pyobj_counts);
    VIR_FREE(pages);
    VIR_FREE(counts);
    return py_retval;
}


static PyObject *
libvirt_virNetworkGetDHCPLeases(PyObject *self ATTRIBUTE_UNUSED,
                                PyObject *args)
{
    PyObject *py_retval = NULL;
    PyObject *py_lease = NULL;
    virNetworkPtr network;
    PyObject *pyobj_network;
    unsigned int flags;
    virNetworkDHCPLeasePtr *leases = NULL;
    int leases_count;
    char *mac = NULL;
    size_t i;

    if (!PyArg_ParseTuple(args, (char *) "OzI:virNetworkDHCPLeasePtr",
                          &pyobj_network, &mac, &flags))
        return NULL;

    network = (virNetworkPtr) PyvirNetwork_Get(pyobj_network);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    leases_count = virNetworkGetDHCPLeases(network, mac, &leases, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (leases_count < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    if (!(py_retval = PyList_New(leases_count)))
        goto no_memory;

    for (i = 0; i < leases_count; i++) {
        virNetworkDHCPLeasePtr lease = leases[i];

        if ((py_lease = PyDict_New()) == NULL)
            goto no_memory;

#define VIR_SET_LEASE_ITEM(NAME, VALUE_OBJ_FUNC)                            \
        do {                                                                \
            PyObject *tmp_val;                                              \
                                                                            \
            if (!(tmp_val = VALUE_OBJ_FUNC))                                \
                goto no_memory;                                             \
                                                                            \
            if (PyDict_SetItemString(py_lease, NAME, tmp_val) < 0) {        \
                Py_DECREF(tmp_val);                                         \
                goto no_memory;                                             \
            }                                                               \
        } while (0)

        VIR_SET_LEASE_ITEM("iface", libvirt_charPtrWrap(lease->iface));
        VIR_SET_LEASE_ITEM("expirytime", libvirt_longlongWrap(lease->expirytime));
        VIR_SET_LEASE_ITEM("type", libvirt_intWrap(lease->type));
        VIR_SET_LEASE_ITEM("mac", libvirt_charPtrWrap(lease->mac));
        VIR_SET_LEASE_ITEM("ipaddr", libvirt_charPtrWrap(lease->ipaddr));
        VIR_SET_LEASE_ITEM("prefix", libvirt_uintWrap(lease->prefix));
        VIR_SET_LEASE_ITEM("hostname", libvirt_charPtrWrap(lease->hostname));
        VIR_SET_LEASE_ITEM("clientid", libvirt_charPtrWrap(lease->clientid));
        VIR_SET_LEASE_ITEM("iaid", libvirt_charPtrWrap(lease->iaid));

#undef VIR_SET_LEASE_ITEM

        if (PyList_SetItem(py_retval, i, py_lease) < 0)
            goto no_memory;

        py_lease = NULL;
    }

 cleanup:
    Py_XDECREF(py_lease);
    if (leases) {
        for (i = 0; i < leases_count; i++)
            virNetworkDHCPLeaseFree(leases[i]);
    }
    VIR_FREE(leases);

    return py_retval;

 no_memory:
    Py_XDECREF(py_retval);
    py_retval = PyErr_NoMemory();
    goto cleanup;
}

#endif /* LIBVIR_CHECK_VERSION(1, 2, 6) */

#if LIBVIR_CHECK_VERSION(1, 2, 8)

static PyObject *
convertDomainStatsRecord(virDomainStatsRecordPtr *records,
                         int nrecords)
{
    PyObject *py_retval;
    PyObject *py_record;
    PyObject *py_record_domain = NULL;
    PyObject *py_record_stats = NULL;
    size_t i;

    if (!(py_retval = PyList_New(nrecords)))
        return NULL;

    for (i = 0; i < nrecords; i++) {
        if (!(py_record = PyTuple_New(2)))
            goto error;

        /* libvirt_virDomainPtrWrap steals the object */
        virDomainRef(records[i]->dom);
        if (!(py_record_domain = libvirt_virDomainPtrWrap(records[i]->dom))) {
            virDomainFree(records[i]->dom);
            goto error;
        }

        if (!(py_record_stats = getPyVirTypedParameter(records[i]->params,
                                                       records[i]->nparams)))
            goto error;

        if (PyTuple_SetItem(py_record, 0, py_record_domain) < 0)
            goto error;

        py_record_domain = NULL;

        if (PyTuple_SetItem(py_record, 1, py_record_stats) < 0)
            goto error;

        py_record_stats = NULL;

        if (PyList_SetItem(py_retval, i, py_record) < 0)
            goto error;

        py_record = NULL;
    }

    return py_retval;

 error:
    Py_XDECREF(py_retval);
    Py_XDECREF(py_record);
    Py_XDECREF(py_record_domain);
    Py_XDECREF(py_record_stats);
    return NULL;
}


static PyObject *
libvirt_virConnectGetAllDomainStats(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval;
    virConnectPtr conn;
    virDomainStatsRecordPtr *records;
    int nrecords;
    unsigned int flags;
    unsigned int stats;

    if (!PyArg_ParseTuple(args, (char *)"OII:virConnectGetAllDomainStats",
                          &pyobj_conn, &stats, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    nrecords = virConnectGetAllDomainStats(conn, stats, &records, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (nrecords < 0)
        return VIR_PY_NONE;

    if (!(py_retval = convertDomainStatsRecord(records, nrecords)))
        py_retval = VIR_PY_NONE;

    virDomainStatsRecordListFree(records);

    return py_retval;
}


static PyObject *
libvirt_virDomainListGetStats(PyObject *self ATTRIBUTE_UNUSED,
                              PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *py_retval;
    PyObject *py_domlist;
    virDomainStatsRecordPtr *records = NULL;
    virDomainPtr *doms = NULL;
    int nrecords;
    int ndoms;
    size_t i;
    unsigned int flags;
    unsigned int stats;

    if (!PyArg_ParseTuple(args, (char *)"OOII:virDomainListGetStats",
                          &pyobj_conn, &py_domlist, &stats, &flags))
        return NULL;

    if (PyList_Check(py_domlist)) {
        ndoms = PyList_Size(py_domlist);

        if (VIR_ALLOC_N(doms, ndoms + 1) < 0)
            return PyErr_NoMemory();

        for (i = 0; i < ndoms; i++)
            doms[i] = PyvirDomain_Get(PyList_GetItem(py_domlist, i));
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    nrecords = virDomainListGetStats(doms, stats, &records, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (nrecords < 0) {
        py_retval = VIR_PY_NONE;
        goto cleanup;
    }

    if (!(py_retval = convertDomainStatsRecord(records, nrecords)))
        py_retval = VIR_PY_NONE;

 cleanup:
    virDomainStatsRecordListFree(records);
    VIR_FREE(doms);

    return py_retval;
}


static PyObject *
libvirt_virDomainBlockCopy(PyObject *self ATTRIBUTE_UNUSED, PyObject *args)
{
    PyObject *pyobj_dom = NULL;
    PyObject *pyobj_dict = NULL;

    virDomainPtr dom;
    char *disk = NULL;
    char *destxml = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    unsigned int flags = 0;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *) "Ozz|OI:virDomainBlockCopy",
                          &pyobj_dom, &disk, &destxml, &pyobj_dict, &flags))
        return VIR_PY_INT_FAIL;

    if (PyDict_Check(pyobj_dict)) {
        if (virPyDictToTypedParams(pyobj_dict, &params, &nparams, NULL, 0) < 0)
            return VIR_PY_INT_FAIL;
    }

    dom = (virDomainPtr) PyvirDomain_Get(pyobj_dom);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainBlockCopy(dom, disk, destxml, params, nparams, flags);
    LIBVIRT_END_ALLOW_THREADS;

    return libvirt_intWrap(c_retval);
}

#endif /* LIBVIR_CHECK_VERSION(1, 2, 8) */

#if LIBVIR_CHECK_VERSION(1, 2, 9)
static PyObject *
libvirt_virNodeAllocPages(PyObject *self ATTRIBUTE_UNUSED,
                                    PyObject *args)
{
    PyObject *pyobj_conn;
    PyObject *pyobj_pages;
    Py_ssize_t size = 0;
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    virConnectPtr conn;
    unsigned int npages = 0;
    unsigned int *pageSizes = NULL;
    unsigned long long *pageCounts = NULL;
    int startCell = -1;
    unsigned int cellCount = 1;
    unsigned int flags = VIR_NODE_ALLOC_PAGES_ADD;
    int c_retval;

    if (!PyArg_ParseTuple(args, (char *)"OOiiI:virNodeAllocPages",
                          &pyobj_conn, &pyobj_pages,
                          &startCell, &cellCount, &flags))
        return NULL;
    conn = (virConnectPtr) PyvirConnect_Get(pyobj_conn);

    if ((size = PyDict_Size(pyobj_pages)) < 0)
        return NULL;

    if (size == 0) {
        PyErr_Format(PyExc_LookupError,
                     "Need non-empty dictionary to pages attribute");
        return NULL;
    }

    if (VIR_ALLOC_N(pageSizes, size) < 0 ||
        VIR_ALLOC_N(pageCounts, size) < 0) {
        PyErr_NoMemory();
        goto error;
    }

    while (PyDict_Next(pyobj_pages, &pos, &key, &value)) {
        if (libvirt_uintUnwrap(key, &pageSizes[npages]) < 0 ||
            libvirt_ulonglongUnwrap(value, &pageCounts[npages]) < 0)
            goto error;
        npages++;
    }

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virNodeAllocPages(conn, npages, pageSizes,
                                 pageCounts, startCell, cellCount, flags);
    LIBVIRT_END_ALLOW_THREADS;

    VIR_FREE(pageSizes);
    VIR_FREE(pageCounts);

    return libvirt_intWrap(c_retval);

 error:
    VIR_FREE(pageSizes);
    VIR_FREE(pageCounts);
    return NULL;
}
#endif /* LIBVIR_CHECK_VERSION(1, 2, 8) */

#if LIBVIR_CHECK_VERSION(1, 2, 11)

static PyObject *
libvirt_virDomainGetFSInfo(PyObject *self ATTRIBUTE_UNUSED, PyObject *args) {
    virDomainPtr domain;
    PyObject *pyobj_domain;
    unsigned int flags;
    virDomainFSInfoPtr *fsinfo = NULL;
    int c_retval, i;
    size_t j;
    PyObject *py_retval = NULL;

    if (!PyArg_ParseTuple(args, (char *)"Oi:virDomainFSInfo",
        &pyobj_domain, &flags))
        return NULL;
    domain = (virDomainPtr) PyvirDomain_Get(pyobj_domain);

    LIBVIRT_BEGIN_ALLOW_THREADS;
    c_retval = virDomainGetFSInfo(domain, &fsinfo, flags);
    LIBVIRT_END_ALLOW_THREADS;

    if (c_retval < 0)
        goto cleanup;

    /* convert to a Python list */
    if ((py_retval = PyList_New(c_retval)) == NULL)
        goto cleanup;

    for (i = 0; i < c_retval; i++) {
        virDomainFSInfoPtr fs = fsinfo[i];
        PyObject *info, *alias;

        if (fs == NULL)
            goto cleanup;
        info = PyTuple_New(4);
        if (info == NULL)
            goto cleanup;
        PyList_SetItem(py_retval, i, info);
        alias = PyList_New(0);
        if (alias == NULL)
            goto cleanup;

        PyTuple_SetItem(info, 0, libvirt_constcharPtrWrap(fs->mountpoint));
        PyTuple_SetItem(info, 1, libvirt_constcharPtrWrap(fs->name));
        PyTuple_SetItem(info, 2, libvirt_constcharPtrWrap(fs->fstype));
        PyTuple_SetItem(info, 3, alias);

        for (j = 0; j < fs->ndevAlias; j++)
            if (PyList_Append(alias,
                              libvirt_constcharPtrWrap(fs->devAlias[j])) < 0)
                goto cleanup;
    }

    for (i = 0; i < c_retval; i++)
        virDomainFSInfoFree(fsinfo[i]);
    VIR_FREE(fsinfo);
    return py_retval;

 cleanup:
    for (i = 0; i < c_retval; i++)
        virDomainFSInfoFree(fsinfo[i]);
    VIR_FREE(fsinfo);
    Py_XDECREF(py_retval);
    return VIR_PY_NONE;
}

#endif /* LIBVIR_CHECK_VERSION(1, 2, 11) */

/************************************************************************
 *									*
 *			The registration stuff				*
 *									*
 ************************************************************************/
static PyMethodDef libvirtMethods[] = {
#include "build/libvirt-export.c"
    {(char *) "virGetVersion", libvirt_virGetVersion, METH_VARARGS, NULL},
    {(char *) "virConnectGetVersion", libvirt_virConnectGetVersion, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(1, 1, 3)
    {(char *) "virConnectGetCPUModelNames", libvirt_virConnectGetCPUModelNames, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 1, 3) */
    {(char *) "virConnectGetLibVersion", libvirt_virConnectGetLibVersion, METH_VARARGS, NULL},
    {(char *) "virConnectOpenAuth", libvirt_virConnectOpenAuth, METH_VARARGS, NULL},
    {(char *) "virConnectListDomainsID", libvirt_virConnectListDomainsID, METH_VARARGS, NULL},
    {(char *) "virConnectListDefinedDomains", libvirt_virConnectListDefinedDomains, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 9, 13)
    {(char *) "virConnectListAllDomains", libvirt_virConnectListAllDomains, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 9, 13) */
    {(char *) "virConnectDomainEventRegister", libvirt_virConnectDomainEventRegister, METH_VARARGS, NULL},
    {(char *) "virConnectDomainEventDeregister", libvirt_virConnectDomainEventDeregister, METH_VARARGS, NULL},
    {(char *) "virConnectDomainEventRegisterAny", libvirt_virConnectDomainEventRegisterAny, METH_VARARGS, NULL},
    {(char *) "virConnectDomainEventDeregisterAny", libvirt_virConnectDomainEventDeregisterAny, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(1, 2, 1)
    {(char *) "virConnectNetworkEventRegisterAny", libvirt_virConnectNetworkEventRegisterAny, METH_VARARGS, NULL},
    {(char *) "virConnectNetworkEventDeregisterAny", libvirt_virConnectNetworkEventDeregisterAny, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 1) */
#if LIBVIR_CHECK_VERSION(0, 10, 0)
    {(char *) "virConnectRegisterCloseCallback", libvirt_virConnectRegisterCloseCallback, METH_VARARGS, NULL},
    {(char *) "virConnectUnregisterCloseCallback", libvirt_virConnectUnregisterCloseCallback, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 0) */
    {(char *) "virStreamEventAddCallback", libvirt_virStreamEventAddCallback, METH_VARARGS, NULL},
    {(char *) "virStreamRecv", libvirt_virStreamRecv, METH_VARARGS, NULL},
    {(char *) "virStreamSend", libvirt_virStreamSend, METH_VARARGS, NULL},
    {(char *) "virDomainGetInfo", libvirt_virDomainGetInfo, METH_VARARGS, NULL},
    {(char *) "virDomainGetState", libvirt_virDomainGetState, METH_VARARGS, NULL},
    {(char *) "virDomainGetControlInfo", libvirt_virDomainGetControlInfo, METH_VARARGS, NULL},
    {(char *) "virDomainGetBlockInfo", libvirt_virDomainGetBlockInfo, METH_VARARGS, NULL},
    {(char *) "virNodeGetInfo", libvirt_virNodeGetInfo, METH_VARARGS, NULL},
    {(char *) "virNodeGetSecurityModel", libvirt_virNodeGetSecurityModel, METH_VARARGS, NULL},
    {(char *) "virDomainGetSecurityLabel", libvirt_virDomainGetSecurityLabel, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 0)
    {(char *) "virDomainGetSecurityLabelList", libvirt_virDomainGetSecurityLabelList, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 0) */
    {(char *) "virNodeGetCPUStats", libvirt_virNodeGetCPUStats, METH_VARARGS, NULL},
    {(char *) "virNodeGetMemoryStats", libvirt_virNodeGetMemoryStats, METH_VARARGS, NULL},
    {(char *) "virDomainGetUUID", libvirt_virDomainGetUUID, METH_VARARGS, NULL},
    {(char *) "virDomainGetUUIDString", libvirt_virDomainGetUUIDString, METH_VARARGS, NULL},
    {(char *) "virDomainLookupByUUID", libvirt_virDomainLookupByUUID, METH_VARARGS, NULL},
    {(char *) "virRegisterErrorHandler", libvirt_virRegisterErrorHandler, METH_VARARGS, NULL},
    {(char *) "virGetLastError", libvirt_virGetLastError, METH_VARARGS, NULL},
    {(char *) "virConnGetLastError", libvirt_virConnGetLastError, METH_VARARGS, NULL},
    {(char *) "virConnectListNetworks", libvirt_virConnectListNetworks, METH_VARARGS, NULL},
    {(char *) "virConnectListDefinedNetworks", libvirt_virConnectListDefinedNetworks, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virConnectListAllNetworks", libvirt_virConnectListAllNetworks, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virNetworkGetUUID", libvirt_virNetworkGetUUID, METH_VARARGS, NULL},
    {(char *) "virNetworkGetUUIDString", libvirt_virNetworkGetUUIDString, METH_VARARGS, NULL},
    {(char *) "virNetworkLookupByUUID", libvirt_virNetworkLookupByUUID, METH_VARARGS, NULL},
    {(char *) "virDomainGetAutostart", libvirt_virDomainGetAutostart, METH_VARARGS, NULL},
    {(char *) "virNetworkGetAutostart", libvirt_virNetworkGetAutostart, METH_VARARGS, NULL},
    {(char *) "virDomainBlockStats", libvirt_virDomainBlockStats, METH_VARARGS, NULL},
    {(char *) "virDomainBlockStatsFlags", libvirt_virDomainBlockStatsFlags, METH_VARARGS, NULL},
    {(char *) "virDomainGetCPUStats", libvirt_virDomainGetCPUStats, METH_VARARGS, NULL},
    {(char *) "virDomainInterfaceStats", libvirt_virDomainInterfaceStats, METH_VARARGS, NULL},
    {(char *) "virDomainMemoryStats", libvirt_virDomainMemoryStats, METH_VARARGS, NULL},
    {(char *) "virNodeGetCellsFreeMemory", libvirt_virNodeGetCellsFreeMemory, METH_VARARGS, NULL},
    {(char *) "virDomainGetSchedulerType", libvirt_virDomainGetSchedulerType, METH_VARARGS, NULL},
    {(char *) "virDomainGetSchedulerParameters", libvirt_virDomainGetSchedulerParameters, METH_VARARGS, NULL},
    {(char *) "virDomainGetSchedulerParametersFlags", libvirt_virDomainGetSchedulerParametersFlags, METH_VARARGS, NULL},
    {(char *) "virDomainSetSchedulerParameters", libvirt_virDomainSetSchedulerParameters, METH_VARARGS, NULL},
    {(char *) "virDomainSetSchedulerParametersFlags", libvirt_virDomainSetSchedulerParametersFlags, METH_VARARGS, NULL},
    {(char *) "virDomainSetBlkioParameters", libvirt_virDomainSetBlkioParameters, METH_VARARGS, NULL},
    {(char *) "virDomainGetBlkioParameters", libvirt_virDomainGetBlkioParameters, METH_VARARGS, NULL},
    {(char *) "virDomainSetMemoryParameters", libvirt_virDomainSetMemoryParameters, METH_VARARGS, NULL},
    {(char *) "virDomainGetMemoryParameters", libvirt_virDomainGetMemoryParameters, METH_VARARGS, NULL},
    {(char *) "virDomainSetNumaParameters", libvirt_virDomainSetNumaParameters, METH_VARARGS, NULL},
    {(char *) "virDomainGetNumaParameters", libvirt_virDomainGetNumaParameters, METH_VARARGS, NULL},
    {(char *) "virDomainSetInterfaceParameters", libvirt_virDomainSetInterfaceParameters, METH_VARARGS, NULL},
    {(char *) "virDomainGetInterfaceParameters", libvirt_virDomainGetInterfaceParameters, METH_VARARGS, NULL},
    {(char *) "virDomainGetVcpus", libvirt_virDomainGetVcpus, METH_VARARGS, NULL},
    {(char *) "virDomainPinVcpu", libvirt_virDomainPinVcpu, METH_VARARGS, NULL},
    {(char *) "virDomainPinVcpuFlags", libvirt_virDomainPinVcpuFlags, METH_VARARGS, NULL},
    {(char *) "virDomainGetVcpuPinInfo", libvirt_virDomainGetVcpuPinInfo, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 0)
    {(char *) "virDomainGetEmulatorPinInfo", libvirt_virDomainGetEmulatorPinInfo, METH_VARARGS, NULL},
    {(char *) "virDomainPinEmulator", libvirt_virDomainPinEmulator, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 0) */
#if LIBVIR_CHECK_VERSION(1, 2, 14)
    {(char *) "virDomainGetIOThreadInfo", libvirt_virDomainGetIOThreadInfo, METH_VARARGS, NULL},
    {(char *) "virDomainPinIOThread", libvirt_virDomainPinIOThread, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 14) */
    {(char *) "virConnectListStoragePools", libvirt_virConnectListStoragePools, METH_VARARGS, NULL},
    {(char *) "virConnectListDefinedStoragePools", libvirt_virConnectListDefinedStoragePools, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virConnectListAllStoragePools", libvirt_virConnectListAllStoragePools, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virStoragePoolGetAutostart", libvirt_virStoragePoolGetAutostart, METH_VARARGS, NULL},
    {(char *) "virStoragePoolListVolumes", libvirt_virStoragePoolListVolumes, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virStoragePoolListAllVolumes", libvirt_virStoragePoolListAllVolumes, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virStoragePoolGetInfo", libvirt_virStoragePoolGetInfo, METH_VARARGS, NULL},
    {(char *) "virStorageVolGetInfo", libvirt_virStorageVolGetInfo, METH_VARARGS, NULL},
    {(char *) "virStoragePoolGetUUID", libvirt_virStoragePoolGetUUID, METH_VARARGS, NULL},
    {(char *) "virStoragePoolGetUUIDString", libvirt_virStoragePoolGetUUIDString, METH_VARARGS, NULL},
    {(char *) "virStoragePoolLookupByUUID", libvirt_virStoragePoolLookupByUUID, METH_VARARGS, NULL},
    {(char *) "virEventRegisterImpl", libvirt_virEventRegisterImpl, METH_VARARGS, NULL},
    {(char *) "virEventAddHandle", libvirt_virEventAddHandle, METH_VARARGS, NULL},
    {(char *) "virEventAddTimeout", libvirt_virEventAddTimeout, METH_VARARGS, NULL},
    {(char *) "virEventInvokeHandleCallback", libvirt_virEventInvokeHandleCallback, METH_VARARGS, NULL},
    {(char *) "virEventInvokeTimeoutCallback", libvirt_virEventInvokeTimeoutCallback, METH_VARARGS, NULL},
    {(char *) "virNodeListDevices", libvirt_virNodeListDevices, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virConnectListAllNodeDevices", libvirt_virConnectListAllNodeDevices, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virNodeDeviceListCaps", libvirt_virNodeDeviceListCaps, METH_VARARGS, NULL},
    {(char *) "virSecretGetUUID", libvirt_virSecretGetUUID, METH_VARARGS, NULL},
    {(char *) "virSecretGetUUIDString", libvirt_virSecretGetUUIDString, METH_VARARGS, NULL},
    {(char *) "virSecretLookupByUUID", libvirt_virSecretLookupByUUID, METH_VARARGS, NULL},
    {(char *) "virConnectListSecrets", libvirt_virConnectListSecrets, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virConnectListAllSecrets", libvirt_virConnectListAllSecrets, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virSecretGetValue", libvirt_virSecretGetValue, METH_VARARGS, NULL},
    {(char *) "virSecretSetValue", libvirt_virSecretSetValue, METH_VARARGS, NULL},
    {(char *) "virNWFilterGetUUID", libvirt_virNWFilterGetUUID, METH_VARARGS, NULL},
    {(char *) "virNWFilterGetUUIDString", libvirt_virNWFilterGetUUIDString, METH_VARARGS, NULL},
    {(char *) "virNWFilterLookupByUUID", libvirt_virNWFilterLookupByUUID, METH_VARARGS, NULL},
    {(char *) "virConnectListNWFilters", libvirt_virConnectListNWFilters, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virConnectListAllNWFilters", libvirt_virConnectListAllNWFilters, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virConnectListInterfaces", libvirt_virConnectListInterfaces, METH_VARARGS, NULL},
    {(char *) "virConnectListDefinedInterfaces", libvirt_virConnectListDefinedInterfaces, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virConnectListAllInterfaces", libvirt_virConnectListAllInterfaces, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
    {(char *) "virConnectBaselineCPU", libvirt_virConnectBaselineCPU, METH_VARARGS, NULL},
    {(char *) "virDomainGetJobInfo", libvirt_virDomainGetJobInfo, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(1, 0, 3)
    {(char *) "virDomainGetJobStats", libvirt_virDomainGetJobStats, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 0, 3) */
    {(char *) "virDomainSnapshotListNames", libvirt_virDomainSnapshotListNames, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 9, 13)
    {(char *) "virDomainListAllSnapshots", libvirt_virDomainListAllSnapshots, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 9, 13) */
    {(char *) "virDomainSnapshotListChildrenNames", libvirt_virDomainSnapshotListChildrenNames, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 9, 13)
    {(char *) "virDomainSnapshotListAllChildren", libvirt_virDomainSnapshotListAllChildren, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 9, 13) */
    {(char *) "virDomainRevertToSnapshot", libvirt_virDomainRevertToSnapshot, METH_VARARGS, NULL},
    {(char *) "virDomainGetBlockJobInfo", libvirt_virDomainGetBlockJobInfo, METH_VARARGS, NULL},
    {(char *) "virDomainSetBlockIoTune", libvirt_virDomainSetBlockIoTune, METH_VARARGS, NULL},
    {(char *) "virDomainGetBlockIoTune", libvirt_virDomainGetBlockIoTune, METH_VARARGS, NULL},
    {(char *) "virDomainSendKey", libvirt_virDomainSendKey, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(1, 0, 3)
    {(char *) "virDomainMigrateGetCompressionCache", libvirt_virDomainMigrateGetCompressionCache, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 0, 3) */
    {(char *) "virDomainMigrateGetMaxSpeed", libvirt_virDomainMigrateGetMaxSpeed, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(1, 1, 0)
    {(char *) "virDomainMigrate3", libvirt_virDomainMigrate3, METH_VARARGS, NULL},
    {(char *) "virDomainMigrateToURI3", libvirt_virDomainMigrateToURI3, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 1, 0) */
    {(char *) "virDomainBlockPeek", libvirt_virDomainBlockPeek, METH_VARARGS, NULL},
    {(char *) "virDomainMemoryPeek", libvirt_virDomainMemoryPeek, METH_VARARGS, NULL},
    {(char *) "virDomainGetDiskErrors", libvirt_virDomainGetDiskErrors, METH_VARARGS, NULL},
#if LIBVIR_CHECK_VERSION(0, 10, 2)
    {(char *) "virNodeGetMemoryParameters", libvirt_virNodeGetMemoryParameters, METH_VARARGS, NULL},
    {(char *) "virNodeSetMemoryParameters", libvirt_virNodeSetMemoryParameters, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(0, 10, 2) */
#if LIBVIR_CHECK_VERSION(1, 0, 0)
    {(char *) "virNodeGetCPUMap", libvirt_virNodeGetCPUMap, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 0, 0) */
#if LIBVIR_CHECK_VERSION(1, 1, 1)
    {(char *) "virDomainCreateXMLWithFiles", libvirt_virDomainCreateXMLWithFiles, METH_VARARGS, NULL},
    {(char *) "virDomainCreateWithFiles", libvirt_virDomainCreateWithFiles, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 1, 1) */
#if LIBVIR_CHECK_VERSION(1, 2, 5)
    {(char *) "virDomainFSFreeze", libvirt_virDomainFSFreeze, METH_VARARGS, NULL},
    {(char *) "virDomainFSThaw", libvirt_virDomainFSThaw, METH_VARARGS, NULL},
    {(char *) "virDomainGetTime", libvirt_virDomainGetTime, METH_VARARGS, NULL},
    {(char *) "virDomainSetTime", libvirt_virDomainSetTime, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 5) */
#if LIBVIR_CHECK_VERSION(1, 2, 6)
    {(char *) "virNodeGetFreePages", libvirt_virNodeGetFreePages, METH_VARARGS, NULL},
    {(char *) "virNetworkGetDHCPLeases", libvirt_virNetworkGetDHCPLeases, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 6) */
#if LIBVIR_CHECK_VERSION(1, 2, 8)
    {(char *) "virConnectGetAllDomainStats", libvirt_virConnectGetAllDomainStats, METH_VARARGS, NULL},
    {(char *) "virDomainListGetStats", libvirt_virDomainListGetStats, METH_VARARGS, NULL},
    {(char *) "virDomainBlockCopy", libvirt_virDomainBlockCopy, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 8) */
#if LIBVIR_CHECK_VERSION(1, 2, 9)
    {(char *) "virNodeAllocPages", libvirt_virNodeAllocPages, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 9) */
#if LIBVIR_CHECK_VERSION(1, 2, 11)
    {(char *) "virDomainGetFSInfo", libvirt_virDomainGetFSInfo, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 11) */
#if LIBVIR_CHECK_VERSION(1, 2, 14)
    {(char *) "virDomainInterfaceAddresses", libvirt_virDomainInterfaceAddresses, METH_VARARGS, NULL},
#endif /* LIBVIR_CHECK_VERSION(1, 2, 14) */
    {NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION > 2
static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
# ifndef __CYGWIN__
        "libvirtmod",
# else
        "cygvirtmod",
# endif
        NULL,
        -1,
        libvirtMethods,
        NULL,
        NULL,
        NULL,
        NULL
};

PyObject *
# ifndef __CYGWIN__
PyInit_libvirtmod
# else
PyInit_cygvirtmod
# endif
  (void)
{
    PyObject *module;

    if (virInitialize() < 0)
        return NULL;

    module = PyModule_Create(&moduledef);

    return module;
}
#else /* ! PY_MAJOR_VERSION > 2 */
void
# ifndef __CYGWIN__
initlibvirtmod
# else
initcygvirtmod
# endif
  (void)
{
    if (virInitialize() < 0)
        return;

    /* initialize the python extension module */
    Py_InitModule((char *)
# ifndef __CYGWIN__
                  "libvirtmod",
# else
                  "cygvirtmod",
# endif
		  libvirtMethods);
}
#endif /* ! PY_MAJOR_VERSION > 2 */
