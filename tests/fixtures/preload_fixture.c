// Harmless shared library used to verify LD_PRELOAD preservation and the
// supervisor's missing-runtime-handshake classification.

int retrace_preload_fixture_marker(void);

int retrace_preload_fixture_marker(void) { return 0; }
