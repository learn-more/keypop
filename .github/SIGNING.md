# Code signing

Release binaries are Authenticode signed with a Certum code signing certificate, through
[learn-more/certum-signing](https://github.com/learn-more/certum-signing). How that works - the cloud HSM, the
SimplySign session, the TOTP login, where to get each value - is documented there, in
[SIGNING.md](https://github.com/learn-more/certum-signing/blob/main/SIGNING.md). This file is only what is
specific to keypop.

**Signing is optional.** Without the settings below a release still builds; it just comes out unsigned and says
so, in an annotation, the job summary, and the release notes. Nothing has to be configured to cut a release.

Only [`release.yml`](workflows/release.yml) signs. `build.yml` does not, on purpose: pull request builds from
forks cannot see secrets anyway, and every signature costs a login to a certificate that is meant for artifacts
people actually download.

## What to store

**Settings → Secrets and variables → Actions → Secrets**

| Secret | What it is |
| --- | --- |
| `CERTUM_USERNAME` | The SimplySign account name. |
| `CERTUM_OTP_URI` | The whole `otpauth://totp/...` URI from SimplySign enrolment. |

**Settings → Secrets and variables → Actions → Variables**

| Variable | Required | Default |
| --- | --- | --- |
| `CERTUM_CERTIFICATE_SHA1` | yes | - |
| `CERTUM_TIMESTAMP_SERVER` | no | `http://time.certum.pl` |
| `CERTUM_INTERMEDIATE_URL` | no | `https://repository.certum.pl/ccsca2021.cer` |

All three of the first are required together; missing any one turns signing off for the whole workflow. The
thumbprint is a variable rather than a secret because a certificate thumbprint is public information, and a wrong
one is much easier to spot in a log than a masked `***`.

These are stored per repository. Renewing the certificate changes `CERTUM_CERTIFICATE_SHA1` here and in every
other repository that signs.

## What gets signed, and in what order

Two files: `keypop-x86.exe` and `keypop-x64.exe`. The PDBs ship alongside them but carry no signature, and
nothing is embedded in either executable, so the ordering is the simple one - build everything, then open the
SimplySign session, then sign. That keeps the session as short as it can be: one TOTP code has to cover the
signing only, not the build in front of it.

1. **Build** both platforms into `build\x86` and `build\x64`.
2. **Stage** the executables and PDBs into `dist\`.
3. **Sign and report** - `certum-signing@v1`, which connects, signs, and then reads the result back off the files.
   The run fails here if signing was configured and did not take.
4. **Hash** everything in `dist\` into `SHA256SUMS.txt`, after signing rewrote the executables.

The name Windows shows in the UAC and SmartScreen dialogs is the `FileDescription` in `src/app.rc`, so the
workflow passes no `description:`.
