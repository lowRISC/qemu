project('otbn', 'rust', version: '0.1.0')

otbn_lib = static_library('otbn',
                          'src/lib.rs',
                          install: true)

otbn_dep = declare_dependency(link_with: otbn_lib)
