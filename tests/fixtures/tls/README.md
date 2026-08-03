# Test-only mutual-TLS identity

TEST ONLY — NOT FOR PRODUCTION.

These files contain a public test CA, two public test certificates, and two unencrypted private keys. The private keys are committed only so the loopback integration test can run unattended. Anybody with the source tree has the keys.

Never install this CA. Never use these certificates or keys for a service, account, host, or production build. Do not copy them to a live Polaris or Sunshine host.

The server certificate is valid only for the test names `localhost` and `127.0.0.1`. Its extended key usage is server authentication. The client certificate has client-authentication usage. The fake server accepts only the exact client leaf. Perigee pins the exact server leaf during the test.

The fake server listens only on an operating-system-assigned port on `127.0.0.1`. Test request history contains only the HTTP method and normalized path. Clipboard request bodies are checked in place and erased immediately.
