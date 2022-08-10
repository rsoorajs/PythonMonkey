#include <gtest/gtest.h>
#include <Python.h>
#include <iostream>
#include <string>

#include <include/TypeEnum.hpp>
#include <include/PyType.hpp>
#include <include/DictType.hpp>
#include <include/StrType.hpp>
#include <include/IntType.hpp>


template<typename Base, typename T>
inline bool instanceof(const T *ptr) {
   return dynamic_cast<const Base*>(ptr) != nullptr;
}

class DictTypeTests : public ::testing::Test {
protected:
    PyObject* dict_type;
    PyObject* default_key;
    PyObject* default_value;
    virtual void SetUp() {      
        Py_Initialize();
        dict_type = PyDict_New();
        default_key = Py_BuildValue("s", (char*)"a");
        default_value = Py_BuildValue("i", 10);

        PyDict_SetItem(dict_type, default_key, default_value);

        Py_XINCREF(dict_type);
        Py_XINCREF(default_key);
        Py_XINCREF(default_value);
    }

    virtual void TearDown() {
        Py_XINCREF(dict_type);
        Py_XINCREF(default_key);
        Py_XINCREF(default_value);
    }
};

TEST_F(DictTypeTests, test_dict_type_instance_of_pytype) { 

    DictType dict = DictType(dict_type);

    EXPECT_TRUE(instanceof<PyType>(&dict));

}

TEST_F(DictTypeTests, test_sets_values_appropriately) {

    DictType dict = DictType(dict_type);

    StrType *key = new StrType((char*)"c");
    IntType *value = new IntType(15);

    dict.set(key, value); 

    PyObject* expected = value->getPyObject();
    PyObject* set_value = PyDict_GetItem(dict.getPyObject(), key->getPyObject());
    
    EXPECT_EQ(set_value, expected);

    delete key;
    delete value;
}
