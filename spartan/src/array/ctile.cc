#include <Python.h>
/* For Numpy C-API */
#define PY_ARRAY_UNIQUE_SYMBOL spartan_ctile_ARRAY_API
#define NO_IMPORT_ARRAY
#include <numpy/arrayobject.h>
#include <iostream>
#include <string>
#include <vector>
#include "ctile.h"
#include "carray_reducer.h"

CTile::CTile(npy_intp dimensions[], int nd, char dtype, 
             CTILE_TYPE tile_type, CTILE_SPARSE_TYPE sparse_type)
{
    int i;

    std::cout << __func__ << " A" << (void*)&PyArray_Type << std::endl;
    initialized = false;
    type = (CTILE_TYPE)tile_type;
    this->sparse_type = (CTILE_SPARSE_TYPE)sparse_type;
    this->nd = nd;
    this->dtype = dtype;
    for (i = 0; i < nd; i++) {
        this->dimensions[i] = dimensions[i];
    }
    dense = NULL;
    mask = NULL;
    for (i = 0; i < 3; i++) {
        sparse[i] = NULL;
    }
    std::cout << __func__ << '1' << std::endl;
}

CTile::CTile(CTile_RPC *rpc)
{
    std::cout << __func__ << " B" << std::endl;
    type = (CTILE_TYPE)rpc->type;
    sparse_type = (CTILE_SPARSE_TYPE)rpc->type;
    initialized = rpc->initialized;
    nd = rpc->nd;
    for (int i = 0; i < rpc->nd; i++) {
        dimensions[i] = rpc->dimensions[i];
    }
    dtype = rpc->item_type;

    if (!initialized) {
        return;
    }

    if (type != CTILE_SPARSE) {
        for (int i = 0; i < 3; i++) {
            sparse[i] = NULL;
        }
        dense = new CArray(rpc->array[0]);
        if (type == CTILE_MASKED) {
            mask = new CArray(rpc->array[1]);
        } else {
            mask = NULL;
        }
    } else {
        dense = mask = NULL;
        for (int i = 0; i < 3; i++) {
            sparse[i] = new CArray(rpc->array[i]);
        }
    }
}

CTile::~CTile()
{
    if (dense != NULL) delete dense;
    if (mask != NULL) delete mask;
    for (int i = 0; i < 3; i++) {
       if (sparse[i] != NULL) delete sparse[i]; 
    }
}

CTile::set_data(CArray *dense, CArray *mask)
{
    if (initialized || type == CTILE_MASKED) {
       return false; 
    }
    this->dense = dense;
    this->mask = mask;
    initialized = true;
    return true;
}

bool
CTile::set_data(CArray **sparse)
{
    if (initialized || type != CTILE_SPARSE) {
       return false; 
    }
    for (int i = 0; i < 3; i++) {
        this->sparse[i] = sparse[i];
    }
    initialized = true;
    return true;
}

void
CTile::initialize(void)
{
    std::cout << __func__ << std::endl;
    for (int i = 0; i < nd ; i++) {
        std::cout << __func__ << " " << dimensions[i] << std::endl;
    }
    if (type == CTILE_SPARSE) {
         sparse[0] = new CArray(dimensions, 1, NPY_INTPLTR);
         sparse[1] = new CArray(dimensions, 1, NPY_INTPLTR);
         sparse[2] = new CArray(dimensions, 1, dtype);
    } else {
        dense = new CArray(dimensions, nd, dtype);
        if (type == CTILE_MASKED) {
            mask = new CArray(dimensions, nd, dtype);
        }
    }
    initialized = true;
}

CExtent*
CTile::slice_to_ex(CSliceIdx &idx) 
{
    return from_slice(idx, dimensions, nd);
}

bool
CTile::is_idx_complete(CSliceIdx &idx)
{
    return is_complete(dimensions, nd, idx);
}

void 
CTile::reduce(CSliceIdx &idx, CTile &update, REDUCER reducer)
{
    CExtent *ex = slice_to_ex(idx);
    bool trivial = is_idx_complete(idx);

    std::cout << __func__ << " reducer = " << (unsigned)reducer << " " << trivial << std::endl;
    if (nd == 0) { // Special case
        scalar_outer_loop(dense, update.dense, reducer);
    } else if (type == CTILE_DENSE || type == CTILE_MASKED) { 
        if ((update.type == CTILE_DENSE && update.type == CTILE_MASKED) ||
             update.type == CTILE_DENSE) { 
            // Don't have to update mask in both cases.
            if (trivial) {
                trivial_dense_outer_loop(dense, update.dense, reducer);
            } else {
                slice_dense_outer_loop(dense, update.dense, ex, reducer);
            }
        } else if (update.type != CTILE_SPARSE) {
            if (trivial) {
                trivial_dense_outer_loop(dense, update.dense, reducer);
                trivial_dense_outer_loop(mask, update.mask, REDUCER_OR);
            } else {
                slice_dense_outer_loop(dense, update.dense, ex, reducer);
                slice_dense_outer_loop(mask, update.mask, ex, REDUCER_OR);
            }
        } else { // SPARSE
            sparse_dense_outer_loop(dense, update.sparse, ex, reducer);
        }
    } else if (type == CTILE_SPARSE) {
        if (update.type == CTILE_DENSE || update.type == CTILE_MASKED) {
            assert(0);
        } else { 
            assert(0);
        }
    }
}

void 
CTile::update(CSliceIdx &idx, CTile &update_data, npy_intp reducer)
{
    std::cout << __func__ << " reducer = " << (unsigned)reducer << std::endl;
    if (!initialized) {
        initialize(); 
    }
    if (reducer >= REDUCER_BEGIN && reducer <= REDUCER_END) {
        reduce(idx, update_data, (REDUCER)reducer);
    } else {
        PyObject *old, *update, *subslice, *reducer_npy;

        PyObject *mod, *object;
        mod = PyImport_ImportModule("spartan.array.tile");
        assert(mod != NULL);
        object = PyObject_GetAttrString(mod, "_internal_update");
        assert(object != NULL);

        old = to_npy();
        update = to_npy();
        subslice = PyTuple_New(nd);
        assert(subslice != NULL);
        for (int i = 0; i < nd; i++) {
            PyTuple_SET_ITEM(subslice, i, 
                             PySlice_New(PyLong_FromLongLong(idx.get_slice(i).start),
                                         PyLong_FromLongLong(idx.get_slice(i).stop),
                                                      NULL));
        }
        reducer_npy = (PyObject*) reducer;
        /* TODO: Do we have to update our dense ? */
        PyObject_CallFunctionObjArgs(object, old, subslice, 
                                     update, reducer_npy, NULL);
    }
}

std::vector <char*>
CTile::get(CSliceIdx &idx)
{
    return to_tile_rpc(idx);
}

std::vector <char*>
CTile::to_tile_rpc(CSliceIdx &idx)
{
    std::cout << __func__ << std::endl;
    std::vector<char*> dest;

    CExtent *ex = slice_to_ex(idx);
    dest.push((char*)(new bool(true)));
    CTile_RPC *rpc = (CTile_RPC*) malloc(sizeof(CTile_RPC));

    rpc->type = type;
    rpc->sparse_type = sparse_type;
    rpc->initialized = initialized;
    rpc->nd = nd;
    rpc->item_type = dtype;
    memcpy(rpc.dimensions, dimensions, sizeof(npy_int64) * NPY_MAXDIMS);
    dest.push((char*)(new NpyMemManager(rpc, rpc, false, sizeof(CTILE_RPC));

    std::cout << __func__ << " A " << std::endl;
    npy_intp size = sizeof(CTile_RPC);
    if (initialized && type != CTILE_SPARSE) {
        std::cout << __func__ << " C " << std::endl;
        npy_intp dense_size = ex->size * dense->get_type_size();

        std::vector v = dense->to_carray_rpc(ex);
        dest.insert(dest.end(), v.begin(), v.end());
        rpc->count = 1;
        if (type == CTILE_MASKED) {
            std::vector v = dense->to_carray_rpc(ex);
            dest.insert(dest.end(), v.begin(), v.end());
            rpc->count = 2;
        }
        std::cout << __func__ << " G " << std::endl;

    } else if (initialized && type == CTILE_SPARSE) {
        if (is_idx_complete(idx)) {
            rpc->count = 3;
            for (int i = 0; i < 3; i++) {
                std::vector v = sparse[i]->to_carray_rpc(ex);
                dest.insert(dest.end(), v.begin(), v.end());
            }
        } else {
            /* TODO: Sparse Slicing ..... */
            assert(0);
            //memcpy(base, &rpc, sizeof(CTile_RPC));
            return NULL;
        }
    }
    return dest;
}

PyObject*
CTile::to_npy(void)
{
    std::cout << "CTile::" << __func__ << std::endl;
    if (!initialized) {
        std::cout << "get a uninitialized tile" << std::endl;
        PyObject *mod, *object, *npy_dimensions;
        npy_dimensions = PyTuple_New(nd);
        assert(npy_dimensions != NULL);
        for (int i = 0; i < nd; i++) {
            PyTuple_SET_ITEM(npy_dimensions, i, PyLong_FromLongLong(dimensions[i]));  
        }
        if (type != CTILE_SPARSE) {
            mod = PyImport_ImportModule("numpy");
            assert(mod != NULL);
            object = PyObject_GetAttrString(mod, "ndarray");
        } else {
            mod = PyImport_ImportModule("scipy.sparse");
            assert(mod != NULL);
            if (sparse_type == CTILE_SPARSE_COO) {
                object = PyObject_GetAttrString(mod, "coo_matrix");
            } else if (sparse_type == CTILE_SPARSE_CSR) {
                object = PyObject_GetAttrString(mod, "csr_matrix");
            } else if (sparse_type == CTILE_SPARSE_CSC) {
                object = PyObject_GetAttrString(mod, "csc_matrix");
            } else {
                object = NULL;
            }
            assert(object != NULL);
        }
        std::cout << "Before creating a matric " << mod << " " << object << std::endl;
        PyObject *ret = PyObject_CallFunctionObjArgs(object, npy_dimensions, NULL);
        std::cout << "After creating a matric" << std::endl;
        return ret;
    } else if (type == CTILE_DENSE) {
        return dense->to_npy();
    } else if (type == CTILE_MASKED) {
        PyObject *npy_dense = dense->to_npy();
        PyObject *npy_mask = mask->to_npy();
        PyObject *mod, *object, *sargs, *kword, *kwds, *ret;

        sargs = PyTuple_New(1);
        assert(sargs != NULL);
        PyTuple_SET_ITEM(sargs, 0, npy_dense);
        kword = PyString_FromString("mask");
        assert(kword != NULL);
        kwds = PyDict_New();
        assert(kwds != NULL);
        PyDict_SetItem(kwds, kword, npy_mask);
        mod = PyImport_ImportModule("numpy.ma");
        assert(mod != NULL);
        object = PyObject_GetAttrString(mod, "array");
        assert(object != NULL);
        ret = PyObject_Call(object, sargs, kwds);
        assert(ret != NULL);
        return ret;
    } else {
        PyObject *npy_sparse[3];
        PyObject *sp, *object, *tuple[3], *sargs, *kargs, *kwords, *ret;

        for (int i = 0; i < 3; i++) {
            npy_sparse[i] = sparse[i]->to_npy(); 
        }

        for (int i = 0; i < 3; i++) {
            tuple[i] = PyTuple_New(2);
            assert(tuple[i] != NULL);
        }
        PyTuple_SET_ITEM(tuple[0], 0, PyLong_FromLongLong(dimensions[0]));
        PyTuple_SET_ITEM(tuple[0], 1, PyLong_FromLongLong(dimensions[1]));
        PyTuple_SET_ITEM(tuple[1], 0, npy_sparse[0]);
        PyTuple_SET_ITEM(tuple[1], 1, npy_sparse[1]);
        PyTuple_SET_ITEM(tuple[2], 0, npy_sparse[2]);
        PyTuple_SET_ITEM(tuple[2], 1, tuple[1]);
        sargs = PyTuple_New(1);
        assert(sargs != NULL);
        PyTuple_SET_ITEM(sargs, 0, tuple[2]);
        kwords = PyString_FromString("shape");
        assert(kwords != NULL);
        kargs = PyDict_New();
        assert(kargs != NULL);
        PyDict_SetItem(kargs, kwords, tuple[0]);
        sp = PyImport_ImportModule("scipy.sparse.coo");
        assert(sp != NULL);
        if (sparse_type == CTILE_SPARSE_COO) {
            object = PyObject_GetAttrString(sp, "coo_matrix");
        } else if (sparse_type == CTILE_SPARSE_CSR) {
            object = PyObject_GetAttrString(sp, "csr_matrix");
        } else if (sparse_type == CTILE_SPARSE_CSC) {
            object = PyObject_GetAttrString(sp, "csc_matrix");
        } else {
            object = NULL;
        }
        assert(object != NULL);
        ret = PyObject_Call(object, sargs, kargs);
        assert(dense != NULL);

        return ret;
    }
}

static long long 
get_longlong(PyObject *o) {
    if (PyNumber_Check(o)) {
        PyObject *_long;
        long long ret;

        _long = PyNumber_Long(o);
        ret = PyLong_AsLongLong(_long);
        Py_DECREF(_long);
        return ret;
    } else {
        assert(0);
    }
    return 0;
}

static CTile*
ctile_creator(PyObject *args)
{
    PyObject *shape, *data;
    const char* dtype;
    unsigned long &tile_type, &sparse_type;

    if (!PyArg_ParseTuple(args, "OskkO", &shape, &dtype, &tile_type, &sparse_type, &data))
        return NULL;

    int nd = PyTuple_Size(shape);
    npy_intp dimensions[NPY_MAXDIMS];
    for (int i = 0; i < nd; i++) {
        dimensions[i] = (npy_intp)get_longlong(PyTuple_GetItem(shape, i));
    }

    CTile *tile = new CTile(dimensions, nd, dtype[0], tile_type, sparse_type);
    if (tile == NULL)
        return NULL;

    if (data != Py_None) {
        assert(PyTuple_Check(data) != 0);
        if (tile_type != CTILE_SPARSE) {
            PyArrayObject *dense = (PyArrayObject*)PyTuple_GetItem(data, 0);
            std::cout << __func__ << "2.1" << std::endl;
            CArray *dense_array = new CArray(dense->dimensions, dense->nd,
                                             dense->descr->type, dense->data,
                                             dense);
            std::cout << __func__ << "2.2" << std::endl;
            CArray *mask_array = NULL;
            if (tile_type == CTILE_MASKED) {
                PyArrayObject *mask = (PyArrayObject*)PyTuple_GetItem(data, 1);
                mask_array = new CArray(mask->dimensions, mask->nd,
                                        mask->descr->type, mask->data,
                                        mask);
                std::cout << __func__ << "2.3" << std::endl;
            }
            std::cout << __func__ << "2.4" << std::endl;
            tile->set_data(dense_array, mask_array);
            std::cout << __func__ << "2.5" << std::endl;
        } else {
            CArray *sparse_array[3];
            for (int i = 0; i < 3; i++) {
                PyArrayObject *sparse = (PyArrayObject*)PyTuple_GetItem(data, i);
                sparse_array[i] = new CArray(sparse->dimensions, sparse->nd,
                                             sparse->descr->type, sparse->data,
                                             sparse);
            }
            tile->set_data(sparse_array);
        }
    }

    return tile;
}
