idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

set(src_dirs        .)
set(include_dirs    . include)
set(requires        espressif__esp-dl spiffs)

set(model_src ${COMPONENT_DIR}/../../training/osnet_reid/outputs/osnet_upperbody.espdl)
set(packed_model ${BUILD_DIR}/espdl_models/reid_model.espdl)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

if (EXISTS ${model_src})
    file(MAKE_DIRECTORY ${BUILD_DIR}/espdl_models)
    set(pack_model_exe ${espdl_dir}/fbs_loader/pack_espdl_models.py)
    add_custom_command(
        OUTPUT ${packed_model}
        COMMENT "Pack OSNet Re-ID model..."
        COMMAND python ${pack_model_exe} --model_path ${model_src} --out_file ${packed_model}
        DEPENDS ${model_src}
        VERBATIM)

    add_custom_target(reid_model_image ALL DEPENDS ${packed_model})
    add_dependencies(flash reid_model_image)
    esptool_py_flash_to_partition(flash "reid_model" ${packed_model})
else()
    message(WARNING "Re-ID model not found: ${model_src}. Run training/export/quantization first.")
endif()
