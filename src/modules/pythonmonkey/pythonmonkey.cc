/**
 * @file pythonmonkey.cc
 * @author Caleb Aikens (caleb@distributive.network)
 * @brief This file defines the pythonmonkey module, along with its various functions.
 * @version 0.1
 * @date 2023-03-29
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "include/modules/pythonmonkey/pythonmonkey.hh"


#include "include/BoolType.hh"
#include "include/setSpiderMonkeyException.hh"
#include "include/DateType.hh"
#include "include/FloatType.hh"
#include "include/FuncType.hh"
#include "include/PyType.hh"
#include "include/pyTypeFactory.hh"
#include "include/StrType.hh"
#include "include/PyEventLoop.hh"

#include <jsapi.h>
#include <jsfriendapi.h>
#include <js/friend/ErrorMessages.h>
#include <js/CompilationAndEvaluation.h>
#include <js/Class.h>
#include <js/Date.h>
#include <js/Initialization.h>
#include <js/Object.h>
#include <js/Proxy.h>
#include <js/SourceText.h>
#include <js/Symbol.h>

#include <Python.h>
#include <datetime.h>

typedef std::unordered_map<PyType *, std::vector<JS::PersistentRooted<JS::Value> *>>::iterator PyToGCIterator;
typedef struct {
  PyObject_HEAD
} NullObject;

std::unordered_map<PyType *, std::vector<JS::PersistentRooted<JS::Value> *>> PyTypeToGCThing; /**< data structure to hold memoized PyObject & GCThing data for handling GC*/

static PyTypeObject NullType = {
  .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "pythonmonkey.null",
  .tp_basicsize = sizeof(NullObject),
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_doc = PyDoc_STR("Javascript null object"),
};

static PyTypeObject BigIntType = {
  .tp_name = "pythonmonkey.bigint",
  .tp_flags = Py_TPFLAGS_DEFAULT
  | Py_TPFLAGS_LONG_SUBCLASS // https://docs.python.org/3/c-api/typeobj.html#Py_TPFLAGS_LONG_SUBCLASS
  | Py_TPFLAGS_BASETYPE,     // can be subclassed
  .tp_doc = PyDoc_STR("Javascript BigInt object"),
  .tp_base = &PyLong_Type,   // extending the builtin int type
};

static void cleanup() {
  delete autoRealm;
  delete global;
  delete JOB_QUEUE;
  if (GLOBAL_CX) JS_DestroyContext(GLOBAL_CX);
  JS_ShutDown();
}

void memoizePyTypeAndGCThing(PyType *pyType, JS::Handle<JS::Value> GCThing) {
  JS::PersistentRooted<JS::Value> *RootedGCThing = new JS::PersistentRooted<JS::Value>(GLOBAL_CX, GCThing);
  PyToGCIterator pyIt = PyTypeToGCThing.find(pyType);

  if (pyIt == PyTypeToGCThing.end()) { // if the PythonObject is not memoized
    std::vector<JS::PersistentRooted<JS::Value> *> gcVector(
      {{RootedGCThing}});
    PyTypeToGCThing.insert({{pyType, gcVector}});
  }
  else {
    pyIt->second.push_back(RootedGCThing);
  }
}

void handleSharedPythonMonkeyMemory(JSContext *cx, JSGCStatus status, JS::GCReason reason, void *data) {
  if (status == JSGCStatus::JSGC_BEGIN) {
    PyToGCIterator pyIt = PyTypeToGCThing.begin();
    while (pyIt != PyTypeToGCThing.end()) {
      PyObject *pyObj = pyIt->first->getPyObject();
      // If the PyObject reference count is exactly 1, then the only reference to the object is the one
      // we are holding, which means the object is ready to be free'd.
      if (_PyGC_FINALIZED(pyObj) || pyObj->ob_refcnt == 1) { // PyObject_GC_IsFinalized is only available in Python 3.9+
        for (JS::PersistentRooted<JS::Value> *rval: pyIt->second) { // for each related GCThing
          bool found = false;
          for (PyToGCIterator innerPyIt = PyTypeToGCThing.begin(); innerPyIt != PyTypeToGCThing.end(); innerPyIt++) { // for each other PyType pointer
            if (innerPyIt != pyIt && std::find(innerPyIt->second.begin(), innerPyIt->second.end(), rval) != innerPyIt->second.end()) { // if the PyType is also related to the GCThing
              found = true;
              break;
            }
          }
          // if this PyObject is the last PyObject that references this GCThing, then the GCThing can also be free'd
          if (!found) {
            delete rval;
          }
        }
        pyIt = PyTypeToGCThing.erase(pyIt);
      }
      else {
        pyIt++;
      }
    }
  }
};

static PyObject *collect(PyObject *self, PyObject *args) {
  JS_GC(GLOBAL_CX);
  Py_RETURN_NONE;
}

static PyObject *asUCS4(PyObject *self, PyObject *args) {
  StrType *str = new StrType(PyTuple_GetItem(args, 0));
  if (!PyUnicode_Check(str->getPyObject())) {
    PyErr_SetString(PyExc_TypeError, "pythonmonkey.asUCS4 expects a string as its first argument");
    return NULL;
  }

  return str->asUCS4();
}

static PyObject *eval(PyObject *self, PyObject *args) {

  StrType *code = new StrType(PyTuple_GetItem(args, 0));
  if (!PyUnicode_Check(code->getPyObject())) {
    PyErr_SetString(PyExc_TypeError, "pythonmonkey.eval expects a string as its first argument");
    return NULL;
  }

  JSAutoRealm ar(GLOBAL_CX, *global);
  JS::CompileOptions options (GLOBAL_CX);
  options.setFileAndLine("noname", 1);

  // initialize JS context
  JS::SourceText<mozilla::Utf8Unit> source;
  if (!source.init(GLOBAL_CX, code->getValue(), strlen(code->getValue()), JS::SourceOwnership::Borrowed)) {
    setSpiderMonkeyException(GLOBAL_CX);
    return NULL;
  }
  delete code;

  // evaluate source code
  JS::Rooted<JS::Value> *rval = new JS::Rooted<JS::Value>(GLOBAL_CX);
  if (!JS::Evaluate(GLOBAL_CX, options, source, rval)) {
    setSpiderMonkeyException(GLOBAL_CX);
    return NULL;
  }

  // translate to the proper python type
  PyType *returnValue = pyTypeFactory(GLOBAL_CX, global, rval);
  if (PyErr_Occurred()) {
    return NULL;
  }

  // TODO: Find a better way to destroy the root when necessary (when the returned Python object is GCed).
  bool rvalIsFunction = rval->isObject() && js::IsFunctionObject(&rval->toObject());
  if (!rvalIsFunction) {  // rval may be a JS function which must be kept alive.
    delete rval;
  }

  if (returnValue) {
    return returnValue->getPyObject();
  }
  else {
    Py_RETURN_NONE;
  }
}

PyMethodDef PythonMonkeyMethods[] = {
  {"eval", eval, METH_VARARGS, "Javascript evaluator in Python"},
  {"collect", collect, METH_VARARGS, "Calls the spidermonkey garbage collector"},
  {"asUCS4", asUCS4, METH_VARARGS, "Expects a python string in UTF16 encoding, and returns a new equivalent string in UCS4. Undefined behaviour if the string is not in UTF16."},
  {NULL, NULL, 0, NULL}
};

struct PyModuleDef pythonmonkey =
{
  PyModuleDef_HEAD_INIT,
  "pythonmonkey",                                   /* name of module */
  "A module for python to JS interoperability", /* module documentation, may be NULL */
  -1,                                           /* size of per-interpreter state of the module, or -1 if the module keeps state in global variables. */
  PythonMonkeyMethods
};

PyObject *SpiderMonkeyError = NULL;

// Implement the `setTimeout` global function
//    https://developer.mozilla.org/en-US/docs/Web/API/setTimeout
//    https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-settimeout
static bool setTimeout(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  // Ensure the first parameter is a function
  // We don't support passing a `code` string to `setTimeout` (yet)
  JS::HandleValue jobArgVal = args.get(0);
  bool jobArgIsFunction = jobArgVal.isObject() && js::IsFunctionObject(&jobArgVal.toObject());
  if (!jobArgIsFunction) {
    JS_ReportErrorNumberASCII(cx, nullptr, nullptr, JSErrNum::JSMSG_NOT_FUNCTION, "The first parameter to setTimeout()");
    return false;
  }

  // Get the function to be executed
  // FIXME (Tom Tang): memory leak, not free-ed
  JS::RootedObject *thisv = new JS::RootedObject(cx, JS::GetNonCCWObjectGlobal(&args.callee())); // HTML spec requires `thisArg` to be the global object
  JS::RootedValue *jobArg = new JS::RootedValue(cx, jobArgVal);
  // `setTimeout` allows passing additional arguments to the callback, as spec-ed
  if (args.length() > 2) { // having additional arguments
    // Wrap the job function into a bound function with the given additional arguments
    //    https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Function/bind
    JS::RootedVector<JS::Value> bindArgs(cx);
    bindArgs.append(JS::ObjectValue(**thisv));
    for (size_t j = 2; j < args.length(); j++) {
      bindArgs.append(args[j]);
    }
    JS::RootedObject jobArgObj = JS::RootedObject(cx, &jobArgVal.toObject());
    JS_CallFunctionName(cx, jobArgObj, "bind", JS::HandleValueArray(bindArgs), jobArg); // jobArg = jobArg.bind(thisv, ...bindArgs)
  }
  // Convert to a Python function
  PyObject *job = pyTypeFactory(cx, thisv, jobArg)->getPyObject();

  // Get the delay time
  //  JS `setTimeout` takes milliseconds, but Python takes seconds
  double delayMs = 0; // use value of 0 if the delay parameter is omitted
  if (args.hasDefined(1)) { JS::ToNumber(cx, args[1], &delayMs); } // implicitly do type coercion to a `number`
  if (delayMs < 0) { delayMs = 0; } // as spec-ed
  double delaySeconds = delayMs / 1000; // convert ms to s

  // Schedule job to the running Python event-loop
  PyEventLoop loop = PyEventLoop::getRunningLoop();
  if (!loop.initialized()) return false;
  PyEventLoop::AsyncHandle handle = loop.enqueueWithDelay(job, delaySeconds);

  // Return the `timeoutID` to use in `clearTimeout`
  args.rval().setDouble((double)PyEventLoop::AsyncHandle::getUniqueId(std::move(handle)));

  return true;
}

// Implement the `clearTimeout` global function
//    https://developer.mozilla.org/en-US/docs/Web/API/clearTimeout
//    https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-cleartimeout
static bool clearTimeout(JSContext *cx, unsigned argc, JS::Value *vp) {
  using AsyncHandle = PyEventLoop::AsyncHandle;
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::HandleValue timeoutIdArg = args.get(0);

  args.rval().setUndefined();

  // silently does nothing when an invalid timeoutID is passed in
  if (!timeoutIdArg.isInt32()) {
    return true;
  }

  // Retrieve the AsyncHandle by `timeoutID`
  int32_t timeoutID = timeoutIdArg.toInt32();
  AsyncHandle *handle = AsyncHandle::fromId((uint32_t)timeoutID);
  if (!handle) return true; // does nothing on invalid timeoutID

  // Cancel this job on Python event-loop
  handle->cancel();

  return true;
}

static JSFunctionSpec jsGlobalFunctions[] = {
  JS_FN("setTimeout", setTimeout, /* nargs */ 2, 0),
  JS_FN("clearTimeout", clearTimeout, 1, 0),
  JS_FS_END
};

PyMODINIT_FUNC PyInit_pythonmonkey(void)
{
  PyDateTime_IMPORT;

  SpiderMonkeyError = PyErr_NewException("pythonmonkey.SpiderMonkeyError", NULL, NULL);
  if (!JS_Init()) {
    PyErr_SetString(SpiderMonkeyError, "Spidermonkey could not be initialized.");
    return NULL;
  }
  Py_AtExit(cleanup);

  GLOBAL_CX = JS_NewContext(JS::DefaultHeapMaxBytes);
  if (!GLOBAL_CX) {
    PyErr_SetString(SpiderMonkeyError, "Spidermonkey could not create a JS context.");
    return NULL;
  }

  JOB_QUEUE = new JobQueue();
  if (!JOB_QUEUE->init(GLOBAL_CX)) {
    PyErr_SetString(SpiderMonkeyError, "Spidermonkey could not create the event-loop.");
    return NULL;
  }

  if (!JS::InitSelfHostedCode(GLOBAL_CX)) {
    PyErr_SetString(SpiderMonkeyError, "Spidermonkey could not initialize self-hosted code.");
    return NULL;
  }

  JS::RealmOptions options;
  static JSClass globalClass = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};
  global = new JS::RootedObject(GLOBAL_CX, JS_NewGlobalObject(GLOBAL_CX, &globalClass, nullptr, JS::FireOnNewGlobalHook, options));
  if (!global) {
    PyErr_SetString(SpiderMonkeyError, "Spidermonkey could not create a global object.");
    return NULL;
  }

  autoRealm = new JSAutoRealm(GLOBAL_CX, *global);

  if (!JS_DefineFunctions(GLOBAL_CX, *global, jsGlobalFunctions)) {
    PyErr_SetString(SpiderMonkeyError, "Spidermonkey could not define global functions.");
    return NULL;
  }

  JS_SetGCCallback(GLOBAL_CX, handleSharedPythonMonkeyMemory, NULL);

  PyObject *pyModule;
  if (PyType_Ready(&NullType) < 0)
    return NULL;
  if (PyType_Ready(&BigIntType) < 0)
    return NULL;

  pyModule = PyModule_Create(&pythonmonkey);
  if (pyModule == NULL)
    return NULL;

  Py_INCREF(&NullType);
  if (PyModule_AddObject(pyModule, "null", (PyObject *)&NullType) < 0) {
    Py_DECREF(&NullType);
    Py_DECREF(pyModule);
    return NULL;
  }
  Py_INCREF(&BigIntType);
  if (PyModule_AddObject(pyModule, "bigint", (PyObject *)&BigIntType) < 0) {
    Py_DECREF(&BigIntType);
    Py_DECREF(pyModule);
    return NULL;
  }

  if (PyModule_AddObject(pyModule, "SpiderMonkeyError", SpiderMonkeyError)) {
    Py_DECREF(pyModule);
    return NULL;
  }
  return pyModule;
}