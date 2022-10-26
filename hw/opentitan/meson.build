# TomCrypt dependency

libtomcrypt_dep = subproject('libtomcrypt').get_variable('libtomcrypt_dep')

# OpenTitan devices

softmmu_ss.add(when: 'CONFIG_OT_ALERT', if_true: files('ot_alert.c'))
softmmu_ss.add(when: 'CONFIG_OT_AST', if_true: files('ot_ast.c'))
softmmu_ss.add(when: 'CONFIG_OT_CSRNG', if_true: files('ot_csrng.c'))
softmmu_ss.add(when: 'CONFIG_OT_EDN', if_true: files('ot_edn.c'))
softmmu_ss.add(when: 'CONFIG_OT_ENTROPY_SRC', if_true: files('ot_entropy_src.c'))
softmmu_ss.add(when: 'CONFIG_OT_FLASH', if_true: files('ot_flash.c'))
softmmu_ss.add(when: 'CONFIG_OT_IBEX_WRAPPER', if_true: files('ot_ibex_wrapper.c'))
softmmu_ss.add(when: 'CONFIG_OT_LIFECYCLE', if_true: files('ot_lifecycle.c'))
softmmu_ss.add(when: 'CONFIG_OT_OTP', if_true: files('ot_otp.c'))
softmmu_ss.add(when: 'CONFIG_OT_PINMUX', if_true: files('ot_pinmux.c'))
softmmu_ss.add(when: 'CONFIG_OT_PWRMGR', if_true: files('ot_pwrmgr.c'))
softmmu_ss.add(when: 'CONFIG_OT_SENSOR', if_true: files('ot_sensor.c'))
