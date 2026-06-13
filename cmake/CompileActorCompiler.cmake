set(ACTORCOMPILER_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler/ActorCompiler.cs
  ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler/ActorParser.cs
  ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler/ParseTree.cs
  ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler/Program.cs
  ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler/Properties/AssemblyInfo.cs)
if(WIN32)
  add_executable(actorcompiler ${ACTORCOMPILER_SRCS})
  target_compile_options(actorcompiler PRIVATE "/langversion:6")
  set_property(TARGET actorcompiler PROPERTY VS_DOTNET_REFERENCES
    "System"
    "System.Core"
    "System.Xml.Linq"
    "System.Data.DataSetExtensions"
    "Microsoft.CSharp"
    "System.Data"
    "System.Xml")
elseif(FDB_USE_PYTHON_CODEGEN)
  # illumos: no C# build; the Python actor compiler is invoked directly in
  # FlowCommands.cmake (python3 -m flow.actorcompiler_py). Provide the target
  # for ordering and an empty actor_exe.
  add_custom_target(actorcompiler DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler_py/__main__.py
    ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler_py/actor_compiler.py
    ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler_py/actor_parser.py
    ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler_py/parse_tree.py
    ${CMAKE_CURRENT_SOURCE_DIR}/flow/actorcompiler_py/errors.py)
  set(actor_exe "")
else()
  set(ACTOR_COMPILER_REFERENCES
    "-r:System,System.Core,System.Xml.Linq,System.Data.DataSetExtensions,Microsoft.CSharp,System.Data,System.Xml")

  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/actorcompiler.exe
    COMMAND ${MCS_EXECUTABLE} ARGS ${ACTOR_COMPILER_REFERENCES} ${ACTORCOMPILER_SRCS} "-target:exe" "-out:actorcompiler.exe"
    DEPENDS ${ACTORCOMPILER_SRCS}
    COMMENT "Compile actor compiler" VERBATIM)
  add_custom_target(actorcompiler DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/actorcompiler.exe)
  set(actor_exe "${CMAKE_CURRENT_BINARY_DIR}/actorcompiler.exe")
endif()
