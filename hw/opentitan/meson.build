# TomCrypt dependency

libtomcrypt_dep = subproject('libtomcrypt').get_variable('libtomcrypt_dep')

# OpenTitan devices

softmmu_ss.add(when: 'CONFIG_OT_ALERT', if_true: files('ot_alert.c'))
