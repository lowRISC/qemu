bitflags_dep = subproject('bitflags').get_variable('bitflags_dep')
paste_dep = subproject('paste').get_variable('paste_dep')
oorandom_dep = subproject('oorandom').get_variable('oorandom_dep')
ethnum_dep = subproject('ethnum-rs').get_variable('ethnum_dep')

if build_machine.system() == 'linux'
    dl_dep = cc.find_library('dl', required: false)
else
    dl_dep = dependency('', required: false)
endif

if build_machine.system() == 'linux'
    dl_dep = cc.find_library('dl', required: false)
else
    dl_dep = dependency('', required: false)
endif

otbn_lib = static_library('otbn',
                          files('otbn/src/lib.rs'),
                          dependencies: [
                            bitflags_dep,
                            paste_dep,
                            oorandom_dep,
                            ethnum_dep,
                            dl_dep],
                          rust_crate_type: 'staticlib')

otbn_dep = declare_dependency(link_with: otbn_lib)

softmmu_ss.add(when: 'CONFIG_OPENTITAN_BIGNUMBER',
               if_true: otbn_dep)
