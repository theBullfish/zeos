/* Zeos aarch64 — backend stubs (M4). Satisfy the linker for the Z+ engine's
 * hardware/crypto chains, which a pure-compute Z+ program never invokes.
 * These get replaced by real ports as drivers come up (net, block, audio, vault). */
#include <stdint.h>

/* chain-type ids (data symbols) */
int CHAIN_AUDIO, CHAIN_BLOCK, CHAIN_FS_EVENT, CHAIN_MDE, CHAIN_NET_RX, CHAIN_NET_TX;

/* signal/chain runtime (efi.h + float-coupled in the x86 tree; stubbed here) */
long chain_count(void)      { return 0; }
long chain_dump(void)       { return 0; }
long chain_get(void)        { return 0; }

/* hardware backends (no drivers on the ARM port yet) */
long block_chain_submit(void)        { return -1; }
long mde_chain_submit(void)          { return -1; }
long net_chain_recv(void)            { return -1; }
long net_chain_send(void)            { return -1; }
long hda_play_pcm(void)              { return -1; }
long http_request(void)              { return -1; }
long fs_event_register_listener(void){ return -1; }
long shell_dispatch_external(void)   { return -1; }
long tls_srv_init(void)              { return -1; }

/* mbedtls (vault/crypto chains) — stubbed until we build the vendored lib */
long mbedtls_aes_crypt_ecb(void)   { return -1; }
void mbedtls_aes_free(void)        { }
void mbedtls_aes_init(void)        { }
long mbedtls_aes_setkey_dec(void)  { return -1; }
long mbedtls_aes_setkey_enc(void)  { return -1; }
long mbedtls_hardware_poll(void)   { return 0; }
long mbedtls_md_hmac(void)         { return -1; }
long mbedtls_md_info_from_type(void){ return 0; }

/* auto-generated remaining backend stubs */
long tls_srv_listen(void){return -1;}
long tod_format(void){return -1;}
long tod_now_millis(void){return -1;}
long tod_now_unix(void){return -1;}
long vault_load_config(void){return -1;}
long vault_read(void){return -1;}
long vault_save_config(void){return -1;}
long vault_size(void){return -1;}
long zp_http_init(void){return -1;}
long zp_http_listen(void){return -1;}
long zp_http_respond(void){return -1;}
long zp_http_route(void){return -1;}
long zp_json_emit_value(void){return -1;}
long zp_json_parse_str(void){return -1;}
long zp_regex_find_str(void){return -1;}
long zp_regex_match_str(void){return -1;}
long zp_regex_replace_str(void){return -1;}

/* auto round2 */
long chain_add_node(void){return -1;}
int CHAIN_CPU;
long chain_create(void){return -1;}
long chain_destroy(void){return -1;}
