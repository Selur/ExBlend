project('ExBlend', 'cpp',
  default_options : ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
  meson_version : '>=0.64.0',
  version : '2.1.0'
)

core_sources = ['src/core/exblend.cpp', 'src/core/anim_deblend.cpp']
sources = core_sources + ['src/avs_plugin.cpp', 'src/vs_plugin.cpp', 'src/vs_anim_deblend.cpp']

# Vendored AviSynth+ (3.7.5) and VapourSynth (API 4) headers.
incs = include_directories('include/avisynth', 'include/vapoursynth', 'src')

cxx = meson.get_compiler('cpp')
if cxx.get_argument_syntax() == 'msvc'
  # compat=1 reproduces the float sums of ExBlend v1.04 bit-exactly: strict single precision.
  add_project_arguments('/fp:precise', '/D_CRT_SECURE_NO_WARNINGS', language : 'cpp')
else
  add_project_arguments('-Wall', '-Wextra', '-ffp-contract=off', language : 'cpp')
  if host_machine.cpu_family() in ['x86', 'x86_64']
    add_project_arguments('-mfpmath=sse', '-msse2', language : 'cpp')
  endif
endif

threads = dependency('threads')

# VapourSynth only autoloads *.dylib on macOS, while Meson defaults to *.so for modules.
module_kwargs = {}
if host_machine.system() == 'darwin'
  module_kwargs += {'name_suffix' : 'dylib'}
endif

if get_option('python_package')
  # Same layout as the VapourSynth PyPI package autoloads from: site-packages/vapoursynth/plugins
  py = import('python').find_installation(pure : false)
  install_dir = join_paths(py.get_install_dir(), 'vapoursynth', 'plugins')
else
  install_dir = join_paths(get_option('prefix'), get_option('libdir'), 'vapoursynth')
endif

shared_module('ExBlend', sources,
  include_directories : incs,
  dependencies : threads,
  install : true,
  install_dir : install_dir,
  gnu_symbol_visibility : 'hidden',
  kwargs : module_kwargs
)

# Pair metrics against the 7-frame window of ExBlend v1.04: meson test -C build
core_test = executable('core_test', ['test/core_test.cpp'] + core_sources,
  include_directories : incs,
  build_by_default : false
)
test('core', core_test)
