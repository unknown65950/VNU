#pragma once
/* vcc_version.h — the version of the VNU C compiler.
 *
 * One definition, used by the compiler itself (`vcc --version`) and by
 * the `vnu` command (`vnu version`), so the two can never disagree.
 * Bump it together with a release of vnu/userspace/vcc.
 */
#define VCC_VERSION "0.5"
