#pragma once
// Copy to unifi_config.h (gitignored). Never commit API keys.
#ifndef UNIFI_HOST
#define UNIFI_HOST ""
#endif
#ifndef UNIFI_API_KEY
#define UNIFI_API_KEY ""
#endif
// Full local origin, for example https://unifi.home.arpa (no trailing slash).
// Key: UniFi Network > Integrations. No UI-account password is used.
#ifndef UNIFI_SITE_ID
#define UNIFI_SITE_ID ""
#endif
#ifndef UNIFI_DEVICE_ID
#define UNIFI_DEVICE_ID ""
#endif
// Empty IDs: select only when exactly one site and one gateway exist.
// Integration UUIDs are not the 'default' string or the console ID in a cloud URL.
#ifndef UNIFI_TLS_SERVER_NAME
#define UNIFI_TLS_SERVER_NAME NULL
#endif
// Optional certificate name when connecting by IP; must match the trusted cert.
// With a private/self-signed CA, add unifi_cert.h using the certificate template.
