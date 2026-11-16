#include "x5/reader.hpp"
#include "reader_holder.hpp"
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
#include <cstring>
namespace py = pybind11;
namespace {
struct Frame {
    py::array_t<uint8_t> image;
    uint64_t index;
    int stream_index;
    int64_t host_receive_ns, host_decode_ns, timestamp_ns;
    explicit Frame(const x5::ReaderFrame& value)
        : image({value.image.rows, value.image.cols, 3}), index(value.index), stream_index(value.stream_index),
          host_receive_ns(value.host_receive_ns), host_decode_ns(value.host_decode_ns), timestamp_ns(value.timestamp_ns) {
        const size_t row_bytes = value.image.cols * 3;
        auto* destination = image.mutable_data();
        for (int row = 0; row < value.image.rows; ++row)
            std::memcpy(destination + row * row_bytes, value.image.ptr(row), row_bytes);
    }
};
}
PYBIND11_MODULE(insta360_x5, m) {
    m.doc() = "X5 native dual-fisheye latest-frame reader (BGR, host monotonic nanoseconds)";
    m.attr("__version__") = X5_VERSION;
    py::register_exception<x5::ReadTimeout>(m, "ReadTimeout", PyExc_TimeoutError);
    py::register_exception<x5::ReaderStopped>(m, "ReaderStopped", PyExc_StopIteration);
    py::class_<Frame>(m, "Frame")
        .def_readonly("image", &Frame::image).def_readonly("index", &Frame::index)
        .def_readonly("stream_index", &Frame::stream_index)
        .def_readonly("host_receive_ns", &Frame::host_receive_ns)
        .def_readonly("host_decode_ns", &Frame::host_decode_ns)
        .def_readonly("timestamp_ns", &Frame::timestamp_ns);
    py::class_<x5::Reader, std::shared_ptr<x5::Reader>>(m, "Reader")
        .def(py::init([](const std::filesystem::path& config) {
            return x5::python::make_reader(x5::load_config(config));
        }), py::arg("config"))
        .def("start", [](x5::Reader& r) -> x5::Reader& { r.start(); return r; },
            py::return_value_policy::reference_internal, py::call_guard<py::gil_scoped_release>())
        .def("stop", &x5::Reader::stop, py::call_guard<py::gil_scoped_release>())
        .def("close", &x5::Reader::stop, py::call_guard<py::gil_scoped_release>())
        .def("latest", [](x5::Reader& r, int index) -> py::object {
            auto frame = r.latest(index);
            return frame ? py::cast(Frame(*frame)) : py::none();
        }, py::arg("stream_index") = 0)
        .def("read", [](x5::Reader& r, std::optional<double> timeout, int index) {
            x5::ReaderFrame frame;
            { py::gil_scoped_release release; frame = r.read(timeout, index); }
            return Frame(frame);
        }, py::arg("timeout") = 1.0, py::arg("stream_index") = 0)
        .def_property_readonly("running", &x5::Reader::running)
        .def_property_readonly("error", [](const x5::Reader& r) -> py::object {
            auto error = r.error(); return error.empty() ? py::none() : py::cast(error);
        })
        .def("__enter__", [](x5::Reader& r) -> x5::Reader& { r.start(); return r; },
            py::return_value_policy::reference_internal, py::call_guard<py::gil_scoped_release>())
        .def("__exit__", [](x5::Reader& r, py::handle, py::handle, py::handle) {
            py::gil_scoped_release release; r.stop(); return false;
        });
}
