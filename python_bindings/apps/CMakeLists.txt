set(SCRIPTS
    bilateral_grid_shell.py
    blur.py
    erode.py
    interpolate.py
    local_laplacian.py)

set(PY_APPS_DEPS_bilateral_grid_shell py_aot_bilateral_grid)
set(PY_APPS_ARGS_bilateral_grid_shell ${TEST_IMAGES_DIR}/images/gray.png ${TEST_TMPDIR}/out.png 0.1 10)

make_shell_path(PY_APPS_PYTHONPATH
                "$<TARGET_FILE_DIR:py_aot_bilateral_grid>"
                "$<TARGET_FILE_DIR:Halide::Python>"
                )
set(TEST_ENV
    "PYTHONPATH=${PY_APPS_PYTHONPATH}"
    "HL_TARGET=${Halide_TARGET}"
    "TEST_TMPDIR=$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}>"
    "TEST_IMAGES_DIR=$<SHELL_PATH:${CMAKE_CURRENT_SOURCE_DIR}/../../apps/images>"
)

foreach (SCRIPT IN LISTS SCRIPTS)
    get_filename_component(BASE ${SCRIPT} NAME_WE)
    add_test(NAME python_apps_${BASE}
             COMMAND Python3::Interpreter "$<SHELL_PATH:${CMAKE_CURRENT_SOURCE_DIR}/${SCRIPT}>" ${PY_APPS_ARGS_${BASE}})
    set_tests_properties(python_apps_${BASE} PROPERTIES
                         LABELS python
                         DEPENDS "${PY_APPS_DEPS_${BASE}}"
                         ENVIRONMENT "${TEST_ENV}")
endforeach ()
