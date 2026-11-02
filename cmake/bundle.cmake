# Merges static libraries into one: cmake -DOUT=<file> -DAR=<tool> -DINPUTS=<a;b;c> -P bundle.cmake
if(NOT OUT OR NOT INPUTS)
    message(FATAL_ERROR "bundle.cmake needs OUT and INPUTS")
endif()

file(REMOVE "${OUT}")

get_filename_component(_tool "${AR}" NAME_WE)
if(_tool STREQUAL "lib" OR _tool STREQUAL "llvm-lib")
    execute_process(COMMAND "${AR}" /nologo "/OUT:${OUT}" ${INPUTS}
                    RESULT_VARIABLE _result)
elseif(APPLE)
    execute_process(COMMAND libtool -static -o "${OUT}" ${INPUTS}
                    RESULT_VARIABLE _result)
else()
    # GNU ar cannot take archives as members, so a script adds them whole.
    set(_script "${OUT}.mri")
    set(_text "CREATE ${OUT}\n")
    foreach(_input ${INPUTS})
        string(APPEND _text "ADDLIB ${_input}\n")
    endforeach()
    string(APPEND _text "SAVE\nEND\n")
    file(WRITE "${_script}" "${_text}")
    execute_process(COMMAND "${AR}" -M INPUT_FILE "${_script}"
                    RESULT_VARIABLE _result)
    file(REMOVE "${_script}")
endif()

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "bundling ${OUT} failed (${_result})")
endif()
