project('flocate', 'cpp', default_options: ['buildtype=debugoptimized','cpp_std=c++17'], version: '1.1.24-dev')

add_project_arguments('-DGROUPNAME="' + get_option('locategroup') + '"', language: 'cpp')
add_project_arguments('-DUPDATEDB_CONF="/etc/updatedb.conf"', language: 'cpp')
dbfile = join_paths(get_option('sharedstatedir'), get_option('dbpath'))
add_project_arguments('-DDBFILE="' + dbfile + '"', language: 'cpp')
add_project_arguments('-DPACKAGE_NAME="flocate"', language: 'cpp')
add_project_arguments('-DPACKAGE_VERSION="' + meson.project_version() + '"', language: 'cpp')
add_project_arguments('-DPACKAGE_BUGREPORT="steinar+flocate@gunderson.no"', language: 'cpp')

cxx = meson.get_compiler('cpp')
uringdep = dependency('liburing', required: false)
zstddep = dependency('libzstd')
threaddep = dependency('threads')
atomicdep = cxx.find_library('atomic', required: false)

if not uringdep.found()
	add_project_arguments('-DWITHOUT_URING', language: 'cpp')
endif
if cxx.has_header('endian.h')
	add_project_arguments('-DHAS_ENDIAN_H', language: 'cpp')
endif

# Function multiversioning is x86-only _and_ not available on certain targets
# (they need “ifunc”, indirect functions, a GNU extension).
code = '''__attribute__((target("default"))) void foo();
__attribute__((target("sse2"))) void foo();
void bar() { foo(); }'''
if cxx.compiles(code, name: 'function multiversioning')
	add_project_arguments('-DHAS_FUNCTION_MULTIVERSIONING', language: 'cpp')
endif

executable('flocate', ['flocate.cpp', 'io_uring_engine.cpp', 'turbopfor.cpp', 'parse_trigrams.cpp', 'serializer.cpp', 'access_rx_cache.cpp', 'needle.cpp', 'complete_pread.cpp'],
	dependencies: [uringdep, zstddep, threaddep, atomicdep],
	install: true,
	install_mode: ['rwxr-sr-x', 'root', get_option('locategroup')])
executable('flocate-build', ['flocate-build.cpp', 'database-builder.cpp'],
	dependencies: [zstddep],
	install: true,
	install_dir: get_option('sbindir'))
updatedb_progname = get_option('updatedb_progname')
executable(updatedb_progname, ['updatedb.cpp', 'database-builder.cpp', 'conf.cpp', 'lib.cpp', 'bind-mount.cpp', 'complete_pread.cpp', 'metadata_hash.cpp'],
	dependencies: [zstddep, threaddep],
	install: true,
	install_dir: get_option('sbindir'))
executable('flocate-showdiff', ['showdiff.cpp', 'metadata_hash.cpp'],
	dependencies: [zstddep],
	install: true,
	install_dir: get_option('sbindir'))

conf_data = configuration_data()
conf_data.set('PROCESSED_BY_MESON', '1')
conf_data.set('sbindir', join_paths(get_option('prefix'), get_option('sbindir')))
conf_data.set('groupname', get_option('locategroup'))
conf_data.set('dbfile', dbfile)
conf_data.set('updatedb_conf', '/etc/updatedb.conf')
conf_data.set('updatedb_progname', updatedb_progname)
update_script = configure_file(input: 'update-flocate.sh',
               output: 'update-flocate.sh',
               configuration: conf_data)

fs = import('fs')
meson.add_install_script('mkdir.sh', fs.parent(dbfile))

if get_option('install_cron')
	install_data(update_script,
		install_dir: '/etc/cron.daily',
		rename: 'flocate')
endif
install_man('flocate.1')
install_man('flocate-build.8')
install_man('flocate-showdiff.1')

updatedb_man = configure_file(input: 'updatedb.8.in',
               output: updatedb_progname + '.8',
               configuration: conf_data)
install_man(updatedb_man)

updatedb_conf_man = configure_file(input: 'updatedb.conf.5.in',
               output: 'updatedb.conf.5',
               configuration: conf_data)
install_man(updatedb_conf_man)

if get_option('install_systemd')
	unitdir = get_option('systemunitdir')
	if unitdir == ''
		systemd = dependency('systemd', required: false)
		if systemd.found()
			unitdir = systemd.get_pkgconfig_variable('systemdsystemunitdir')
		endif
	endif
	if unitdir != ''
		updatedb_service = configure_file(input: 'flocate-updatedb.service.in',
			output: 'flocate-updatedb.service',
			configuration: conf_data)
		install_data(updatedb_service, install_dir: unitdir)
		install_data('flocate-updatedb.timer', install_dir: unitdir)
	endif
endif

# Requires having TurboPFor checked out, so not built by default.
# Unless you have a recent Meson, there's no apparently good way
# of calling into that directory to run make, and we're not
# replicating their build system, so it has to be manual.
pfordir = meson.source_root() + '/TurboPFor-Integer-Compression'
if run_command('[', '-r', pfordir + '/libic.a', ']', check: false).returncode() == 0
	turbopfordep = declare_dependency(
		include_directories: include_directories('TurboPFor-Integer-Compression'),
		dependencies: meson.get_compiler('cpp').find_library('ic', dirs: pfordir))
	executable('bench', ['bench.cpp', 'io_uring_engine.cpp', 'turbopfor.cpp', 'complete_pread.cpp'],
		dependencies: [uringdep, turbopfordep],
		build_by_default: false,
		install: false)
endif
