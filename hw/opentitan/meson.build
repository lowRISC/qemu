# TomCrypt dependency

libtomcrypt_dep = subproject('libtomcrypt').get_variable('libtomcrypt_dep')

# OpenTitan devices

softmmu_ss.add(when: 'CONFIG_OT_ALERT', if_true: files('ot_alert.c'))
softmmu_ss.add(when: 'CONFIG_OT_AST', if_true: files('ot_ast.c'))
softmmu_ss.add(when: 'CONFIG_OT_OTP', if_true: files('ot_otp.c'))
