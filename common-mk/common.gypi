# This file is included for every single target.
# Use it to define Platform2 wide settings and defaults.

{
  'variables': {
    # Set this to 0 if you'd like to disable -Werror.
    'enable_werror%': 1,

    # Set this to 1 if you'd like to enable debugging flags.
    'debug%': 0,

    # Set this to 0 if you'd like to disable -clang-syntax.
    'clang_syntax%': 1,

    # Set this to 1 if your C++ code throws or catches exceptions.
    'enable_exceptions%': 0,

    # DON'T EDIT BELOW THIS LINE.
    # You can declare new variables, but you shouldn't be changing their
    # default settings.  The platform python build helpers set them up.
    'USE_attestation%': 0,
    'USE_cellular%': 0,
    'USE_cros_host%': 0,
    'USE_feedback%': 0,
    'USE_gdmwimax%': 0,
    'USE_opengles%': 0,
    'USE_passive_metrics%': 0,
    'USE_power_management%': 0,
    'USE_profile%': 0,
    'USE_tcmalloc%': 0,
    'USE_test%': 0,
    'USE_vpn%': 0,
    'USE_wake_on_wifi%': 0,
    'USE_wimax%': 0,

    'external_cflags%': '',
    'external_cxxflags%': '',
    'external_cppflags%': '',
    'external_ldflags%': '',

    'deps%': '',
    'libdir%': '/usr/lib',

    'cflags_no_exceptions': [
      '-fno-exceptions',
      '-fno-unwind-tables',
      '-fno-asynchronous-unwind-tables',
    ],
  },
  'conditions': [
    ['USE_cros_host == 1', {
      'variables': {
        'pkg-config': 'pkg-config',
        'sysroot': '',
      },
    }],
  ],
  'target_defaults': {
    'include_dirs': [
      '<(INTERMEDIATE_DIR)/include',
      '<(SHARED_INTERMEDIATE_DIR)/include',
      '..',             # src/platform2
      '../../platform', # src/platform
    ],
    'cflags': [
      '-Wall',
      '-Wno-psabi',
      '-ggdb3',
      '-fstack-protector-strong',
      '-Wformat=2',
      '-fvisibility=internal',
      '-Wa,--noexecstack',
    ],
    'cflags_c': [
      '<@(external_cppflags)',
      '<@(external_cflags)',
    ],
    'cflags_cc': [
      '-std=gnu++11',
      '<@(external_cppflags)',
      '<@(external_cxxflags)',
    ],
    'link_settings': {
      'ldflags': [
        '<(external_ldflags)',
        '-Wl,-z,relro',
        '-Wl,-z,noexecstack',
        '-Wl,-z,now',
        '-Wl,--as-needed',
      ],
    },
    'conditions': [
      ['enable_werror == 1', {
        'cflags+': [
          '-Werror',
        ],
      }],
      ['USE_cros_host == 1', {
        'defines': [
          'NDEBUG',
        ],
      }],
      ['USE_cros_host == 0 and clang_syntax == 1', {
        'cflags': [
          '-clang-syntax',
        ],
      }],
      ['enable_exceptions == 0', {
        'cflags_cc': [
          '<@(cflags_no_exceptions)',
        ],
        'cflags_c': [
          '<@(cflags_no_exceptions)',
        ],
      }],
      ['enable_exceptions == 1', {
        # Remove the no-exceptions flags from both cflags_cc and cflags_c.
        'cflags_cc!': [
          '<@(cflags_no_exceptions)',
        ],
        'cflags_c!': [
          '<@(cflags_no_exceptions)',
        ],
      }],
      ['USE_cros_host == 0', {
        'include_dirs': [
          '<(sysroot)/usr/include',
        ],
        'cflags': [
          '--sysroot=<(sysroot)',
        ],
        'link_settings': {
          'ldflags': [
            '--sysroot=<(sysroot)',
          ],
        },
      }],
      ['deps != ""', {
        'cflags+': [
          # We don't care about getting rid of duplicate cflags, so we use
          # @ to expand to list.
          '>!@(<(pkg-config) >(deps) --cflags)',
        ],
        'link_settings': {
          # We don't care about getting rid of duplicate ldflags, so we use
          # @ to expand to list.
          'ldflags+': [
            '>!@(<(pkg-config) >(deps) --libs-only-L --libs-only-other)',
          ],
          'libraries+': [
            # Note there's no @ here intentionally, we want to keep libraries
            # returned by pkg-config as a single string in order to maintain
            # order for linking (rather than a list of individual libraries
            # which GYP would make unique for us).
            '>!(<(pkg-config) >(deps) --libs-only-l)',
          ],
        },
      }],
      ['USE_tcmalloc == 1', {
        'link_settings': {
          'libraries': [
            '-ltcmalloc',
          ],
        },
      }],
    ],
    'target_conditions': [
      ['_type == "executable"', {
        'cflags': ['-fPIE'],
        'ldflags': ['-pie'],
      }],
      ['_type == "static_library"', {
        'cflags': ['-fPIE'],
      }],
      ['_type == "shared_library"', {
        'cflags': ['-fPIC'],
      }],
      ['OS!="win"', {
        'sources/': [ ['exclude', '_win(_browsertest|_unittest)?\\.(h|cc)$'],
                      ['exclude', '(^|/)win/'],
                      ['exclude', '(^|/)win_[^/]*\\.(h|cc)$'] ],
      }],
      ['OS!="mac"', {
        'sources/': [ ['exclude', '_(cocoa|mac)(_unittest)?\\.(h|cc|mm?)$'],
                      ['exclude', '(^|/)(cocoa|mac)/'] ],
      }],
      ['OS!="ios"', {
        'sources/': [ ['exclude', '_ios(_unittest)?\\.(h|cc|mm?)$'],
                      ['exclude', '(^|/)ios/'] ],
      }],
      ['(OS!="mac" and OS!="ios")', {
        'sources/': [ ['exclude', '\\.mm?$' ] ],
      }],
    ],
  },
}
