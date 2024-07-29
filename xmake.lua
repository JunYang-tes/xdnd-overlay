add_rules("plugin.compile_commands.autoupdate",{outputdir="./"})
add_rules("mode.debug", "mode.release")
add_requires("lua5.3")
add_requires("x11")

target("xdnd-overlay")
    set_kind("shared")
    set_symbols("debug")
    add_headerfiles("./uthash/src/*h")
    add_files("*.c")
    add_packages("lua5.3")
    add_packages("x11")
target_end() 

