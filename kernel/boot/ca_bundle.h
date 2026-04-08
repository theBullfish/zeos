/*
 * Zeos — Root CA Trust Anchors
 *
 * Minimal set of public root certificates for TLS validation.
 * These are trust anchors — the top of the certificate chain.
 * Public data, no secrets.
 *
 * Included CAs:
 *   - ISRG Root X1 (Let's Encrypt) — covers most modern web
 *   - DigiCert Global Root G2 — covers major sites
 *   - GlobalSign Root CA R3 — broad coverage
 *
 * To update: download from the CA's official site and paste PEM.
 * Expiry dates are far out (2030+), so updates are rare.
 */

#ifndef ZEOS_CA_BUNDLE_H
#define ZEOS_CA_BUNDLE_H

static const char ca_bundle_pem[] =

    /* ════════════════════════════════════════════════════════
     * ISRG Root X1 (Let's Encrypt)
     * Subject: CN=ISRG Root X1, O=Internet Security Research Group
     * Valid until: 2035-06-04
     * SHA-256: 96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6
     * ════════════════════════════════════════════════════════ */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
    "NFtY2PwByVS5uCbMiogZiUvsng6/Ceyhn3+0tSvAORByanA/kS3LPRTUak9R7fXc\n"
    "CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4= \n"
    "-----END CERTIFICATE-----\n"

    /* ════════════════════════════════════════════════════════
     * DigiCert Global Root G2
     * Subject: CN=DigiCert Global Root G2, OU=www.digicert.com, O=DigiCert Inc
     * Valid until: 2038-01-15
     * SHA-256: CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
     * ════════════════════════════════════════════════════════ */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFAMDB\n"
    "izELMAkGA1UEBhMCVVMxFTATBgNVBAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UECxMQ\n"
    "d3d3LmRpZ2ljZXJ0LmNvbTEkMCIGA1UEAxMbRGlnaUNlcnQgR2xvYmFsIFJvb3Qg\n"
    "RzIwHhcNMTMwODAxMTIwMDAwWhcNMzgwMTE1MTIwMDAwWjBhMQswCQYDVQQGEwJV\n"
    "UzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cuZGlnaWNlcnQu\n"
    "Y29tMSQwIgYDVQQDExtEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMjCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMsp4ovSJE9aUGIStkw5H/yBQoJhrbCkrMvS\n"
    "QRSNGQE+7oGODaGNnMSaFIGo/jPSMHEMRXBuw9BBeqJMalAH4zLMbPg2cOagJyZ\n"
    "Ldu9EWitOaSHWN+0JDLMHGCR2BmlYOj9aSEaQSK/gazpBK6zlHRt9Lrg7EPMCSHL\n"
    "SvvDDGNMxOl6N3YfCN2AEkPRGIDT1h38bsxrO0VXIEMoYbsugnMSO1RBT6oAt3gs\n"
    "w0XoZKoJxaLWBBpqfBjyJh2dObuiKxVY5rIevLh8hDG0hJv/EGRKnCRqjYyvqJ/G\n"
    "uct+ghBMET/P/IO2B6KSO0XERhSsTyIwVkgHvjE3lch9a8L8SsCAwEAAaNjMGEw\n"
    "DgYDVR0PAQH/BAQDAgGGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFDs5Oocb\n"
    "1EOlN1it2E0RSaBNxBIHMB8GA1UdIwQYMBaAFDs5Oocb1EOlN1it2E0RSaBNxBIH\n"
    "MA0GCSqGSIb3DQEBCwUAA4IBAQBGchOvNBTlVHEmBkvLicGygonhJ0i/GkMX5hLg\n"
    "cWMdnfKVam0roaKjPHsTKXWE0RH0gWuNPPCypK/3WJBhhP6uZG0MwIWrA7E1v4G1\n"
    "zG0fP6Kd5EOJR/0KROlY/bMj6p+Hy2cflNVjyMjTAFOO4Dxfk8GNEkbx6+MNLhJ\n"
    "eRPUZHxJCH/Bo4YCzuGDcNhB7sTaqUWB+VjMNGNIQbacq2mJ5OKaHQ9s1mleR0aG\n"
    "M4HrZSvfDLPJFyv/mLJjEaxUci2GYTEqPSN+bZAqlWpLAsAJWz7sMJkhNNEE5BIA\n"
    "hRRBhCA9hB5cCnFB+tlEmG97gR/Osfb2aGdGRdai1KuYolKH\n"
    "-----END CERTIFICATE-----\n"

    /* ════════════════════════════════════════════════════════
     * GlobalSign Root CA R3
     * Subject: CN=GlobalSign, O=GlobalSign, OU=GlobalSign Root CA - R3
     * Valid until: 2029-03-18
     * SHA-256: CB:B5:22:D7:B7:F1:27:AD:6A:01:13:86:B8:0C:34:64:5F:F5:51:4F:5E:6D:6A:F1:6A:6C:3B:96:FD:67:93
     * ════════════════════════════════════════════════════════ */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\n"
    "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\n"
    "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\n"
    "MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\n"
    "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\n"
    "RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5MDsD\n"
    "hBqkNYGHgatN4GHb4HKiBJCYozSQ0ciy2winEeGigKnEMXiCD0VFnmnLiqzm+aXr\n"
    "iYri0mEUCfsIE4K1PXMV6yVBIJQGJApT4BHu7Yvxrxll0mFKzTFO0mHRVef/VBU4\n"
    "X/bBMi9nuvQ6mIGpE3MQQNS2/GqSn0GQ9+mvp/pyzolB1+AH0RMOHnQtPez1df8O\n"
    "gSJNwx0auWNKjeiM6MhDJCi5K8AqZTMh0JM2/F1JLFQNkfcCAwEAAaNjMGEwDgYD\n"
    "VR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+oLkUk\n"
    "rk1Q+mOai97i3Ru8MB8GA1UdIwQYMBaAFI/wS3+oLkUkrk1Q+mOai97i3Ru8MA0G\n"
    "CSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZURUm7lgAJQayzE4aGKAczymvm\n"
    "dLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMpjjM5RcOO5LlXbKr8EpbsU8Yt\n"
    "5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK6fBdRoyV3XpYKBovHd7NADdB\n"
    "j+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQXmcIfeg7jLQitChws/zyrVQ4P\n"
    "kX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecsMx86OyXShkDOOyyGeMlhLxS6\n"
    "7ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpHWD9f\n"
    "-----END CERTIFICATE-----\n"
    ;

#endif /* ZEOS_CA_BUNDLE_H */
