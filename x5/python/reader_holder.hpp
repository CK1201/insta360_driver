#pragma once
#include "x5/reader.hpp"
#include <pybind11/pybind11.h>

namespace x5::python {
template<class... Args>
std::shared_ptr<Reader> make_reader(Args&&... args) {
    // Final Python-reference deletion also joins the native capture worker.
    return std::shared_ptr<Reader>(new Reader(std::forward<Args>(args)...), [](Reader* reader) {
        if (Py_IsInitialized() && PyGILState_Check()) {
            pybind11::gil_scoped_release release;
            delete reader;
        } else delete reader;
    });
}
}
