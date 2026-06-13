if(WIN32)
  # C# is currently only supported on Windows.
  # On other platforms we find mono manually
  enable_language(CSharp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  # illumos has no mono in pkgsrc, so use the bundled Python codegen tools
  # (flow/actorcompiler_py + fdbclient/vexillographer/vexillographer.py),
  # matching 8.0's mono-free build. The C# coverage tool is skipped as
  # non-essential metadata (see FlowCommands.cmake).
  set(FDB_USE_PYTHON_CODEGEN ON CACHE INTERNAL "Use the bundled Python codegen tools")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
else()
  # for other platforms we currently use mono

  find_program(MONO_EXECUTABLE mono)
  if (NOT MONO_EXECUTABLE)
    message(FATAL_ERROR "Could not find 'mono' executable!")
  endif()

  find_program(MCS_EXECUTABLE mcs)
  if (NOT MCS_EXECUTABLE)
    message(FATAL_ERROR "Could not find 'mcs' executable, which is part of Mono!")
  endif()
endif()
