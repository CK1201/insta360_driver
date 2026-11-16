#include "go3s/reader.hpp"
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <cstring>
namespace py = pybind11;
namespace {
struct PythonFrame {
    py::array_t<uint8_t> image;
    uint64_t index;
    int stream_index;
    int64_t host_receive_ns, host_decode_ns, timestamp_ns;
};
PythonFrame copy_frame(const go3s::ReaderFrame& frame) {
    py::array_t<uint8_t> image({frame.image.rows,frame.image.cols,3});
    auto* output=image.mutable_data();
    const size_t row_bytes=static_cast<size_t>(frame.image.cols)*3;
    for(int row=0;row<frame.image.rows;++row) std::memcpy(output+row*row_bytes,frame.image.ptr(row),row_bytes);
    return {std::move(image),frame.index,frame.stream_index,frame.host_receive_ns,frame.host_decode_ns,frame.timestamp_ns};
}
}
PYBIND11_MODULE(_native,m) {
    m.doc()="GO3S native capture with a single latest decoded frame slot";
    py::register_exception<go3s::ReaderTimeout>(m,"TimeoutError",PyExc_TimeoutError);
    py::register_exception<go3s::ReaderEnded>(m,"StopIteration",PyExc_StopIteration);
    py::class_<PythonFrame>(m,"Frame")
        .def_readonly("image",&PythonFrame::image)
        .def_readonly("index",&PythonFrame::index)
        .def_readonly("stream_index",&PythonFrame::stream_index)
        .def_readonly("host_receive_ns",&PythonFrame::host_receive_ns)
        .def_readonly("host_decode_ns",&PythonFrame::host_decode_ns)
        .def_readonly("timestamp_ns",&PythonFrame::timestamp_ns);
    py::class_<go3s::Reader,std::shared_ptr<go3s::Reader>>(m,"Reader")
        .def(py::init([](const std::string& config) {
            return std::shared_ptr<go3s::Reader>(new go3s::Reader(config),[](go3s::Reader* reader) {
                // Destruction also joins native workers; let Python bridge/consumer
                // threads run exactly as for explicit stop/close.
                if(Py_IsInitialized() && PyGILState_Check()) { py::gil_scoped_release release; delete reader; }
                else delete reader;
            });
        }),py::arg("config")="config/go3s.yaml")
        .def("start",&go3s::Reader::start,py::return_value_policy::reference_internal,py::call_guard<py::gil_scoped_release>())
        .def("stop",&go3s::Reader::stop,py::call_guard<py::gil_scoped_release>())
        .def("close",&go3s::Reader::stop,py::call_guard<py::gil_scoped_release>())
        .def("__enter__",&go3s::Reader::start,py::return_value_policy::reference_internal,py::call_guard<py::gil_scoped_release>())
        .def("__exit__",[](go3s::Reader& reader,py::object,py::object,py::object) {
            py::gil_scoped_release release; reader.stop();
        })
        .def("latest",[](go3s::Reader& reader,int stream_index)->py::object {
            auto frame=reader.latest(stream_index);
            return frame?py::cast(copy_frame(*frame)):py::none();
        },py::arg("stream_index")=0)
        .def("read",[](go3s::Reader& reader,std::optional<double> timeout,int stream_index) {
            go3s::ReaderFrame frame;
            { py::gil_scoped_release release; frame=reader.read(timeout,stream_index); }
            return copy_frame(frame);
        },py::arg("timeout")=1.0,py::arg("stream_index")=0)
        .def_property_readonly("running",&go3s::Reader::running)
        .def_property_readonly("error",&go3s::Reader::error);
}
