SLSsteam has a toggleable API. If it's enabled you can write commands into `$XDG_RUNTIME_DIR/SLSsteam/api` (`~/.cache/SLSsteam/api` without `$XDG_RUNTIME_DIR`)

API commands are split by using | as a seperator. So for example to install spacewar to library 0 you can write install|480|0 into that file

Arguments written in square brackets \[\] are optional

Results are written to the log (`~/.SLSsteam.log`) at the Info level

The current commands are:

### Application Manager
dumplibraries : Lists all available libraries' label, their path & their index

install|appId|libraryIndex : Installs appId into library at libraryIndex

uninstall|appId : Uninstalls appId

### Compatibility Manager
getcompat|appId : Lists the current compatibility tool for appId

setcompat|appId\[|tool-name\] : Set's or clears the compatibility tool for appId
