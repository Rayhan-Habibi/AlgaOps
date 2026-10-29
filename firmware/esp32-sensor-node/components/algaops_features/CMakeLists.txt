# Vibration features: the C core's implementation, compiled for the node so
# that firmware and gateway compute RMS / peak-to-peak with the same code.
set(core_dir "${CMAKE_CURRENT_LIST_DIR}/../../../../algaops-assurance")
idf_component_register(SRCS "${core_dir}/src/features.c"
                       INCLUDE_DIRS "${core_dir}/include")
