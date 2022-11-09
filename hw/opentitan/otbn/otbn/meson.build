project('otbn', 'rust', version: '0.1.0')

oorandom_sp = subproject('oorandom')
oorandom_dep = oorandom_sp.get_variable('oorandom_dep')

otbn_lib = static_library('otbn',
                          'src/lib.rs',
                          link_with: oorandom_lib,
                          install: true)

otbn_dep = declare_dependency(link_with: otbn_lib)
