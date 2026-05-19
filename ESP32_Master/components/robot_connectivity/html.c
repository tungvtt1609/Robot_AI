#include "html.h"

#include <stdlib.h>
#include <string.h>

char *htmlShowResponse(void)
{
    return "<!DOCTYPE html>"
           "<html lang='en'><head>"
           "<meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
           "<title>AI Robot - Saved</title>"
           "<style>"
           ":root{--bg1:#06121f;--bg2:#12385e;--card:#f5fbff;--ink:#0e2238;--accent:#0ea5a4;--ok:#18a34a;}"
           "*{box-sizing:border-box}"
           "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:20px;"
           "font-family:'Trebuchet MS','Segoe UI',Verdana,sans-serif;"
           "background:radial-gradient(1100px 500px at -10% -20%,#2a6ea1 0%,transparent 55%),"
           "linear-gradient(160deg,var(--bg1),var(--bg2));color:var(--ink)}"
           ".card{width:min(560px,100%);background:var(--card);border-radius:20px;padding:28px 24px;"
           "box-shadow:0 18px 45px rgba(2,8,20,.35)}"
           ".badge{display:inline-block;padding:6px 12px;border-radius:999px;"
           "background:rgba(24,163,74,.14);color:var(--ok);font-weight:700;font-size:12px;letter-spacing:.3px}"
           "h1{margin:12px 0 8px;font-size:28px;line-height:1.2}"
           "p{margin:0;color:#294869;line-height:1.6}"
           "</style></head><body>"
           "<section class='card'>"
           "<span class='badge'>CONFIGURATION SAVED</span>"
           "<h1>Wi-Fi credentials received.</h1>"
           "<p>The robot is restarting and will reconnect using the new network settings. "
           "You can close this page now.</p>"
           "</section></body></html>";
}

char *generateHTMLSetUp(char *wifiList)
{
    const char *head =
        "<!DOCTYPE html>"
        "<html lang='en'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>AI Robot Wi-Fi Setup</title>"
        "<style>"
        ":root{--bg1:#06121f;--bg2:#12385e;--card:#f5fbff;--ink:#0e2238;--muted:#43617f;"
        "--accent:#0ea5a4;--accent2:#f57c32;--line:#d5e4f3;}"
        "*{box-sizing:border-box}"
        "body{margin:0;min-height:100vh;padding:20px;display:grid;place-items:center;"
        "font-family:'Trebuchet MS','Segoe UI',Verdana,sans-serif;"
        "background:radial-gradient(1200px 620px at 15% -15%,#2d6f9f 0%,transparent 52%),"
        "radial-gradient(900px 450px at 100% 120%,#0ea5a455 0%,transparent 55%),"
        "linear-gradient(155deg,var(--bg1),var(--bg2));color:var(--ink)}"
        ".panel{width:min(640px,100%);background:var(--card);border-radius:22px;overflow:hidden;"
        "box-shadow:0 18px 48px rgba(2,8,20,.35)}"
        ".hero{padding:20px 22px;background:linear-gradient(120deg,#d8f4f4,#fff1e8)}"
        ".hero h1{margin:0 0 8px;font-size:30px;line-height:1.1}"
        ".hero p{margin:0;color:#2f4a66;line-height:1.55}"
        "form{padding:20px 22px 24px}"
        ".grid{display:grid;gap:14px}"
        "label{display:block;font-weight:700;font-size:13px;letter-spacing:.2px;margin:0 0 6px}"
        "input,select{width:100%;height:46px;border-radius:12px;border:1px solid var(--line);"
        "padding:0 12px;background:#fff;color:var(--ink);font-size:15px}"
        "input:focus,select:focus{outline:none;border-color:var(--accent);"
        "box-shadow:0 0 0 4px #0ea5a420}"
        ".actions{margin-top:18px;display:flex;gap:12px;flex-wrap:wrap}"
        "button{height:46px;border:0;border-radius:12px;padding:0 18px;font-weight:700;"
        "cursor:pointer;transition:transform .1s ease,filter .2s ease}"
        ".btn-main{background:linear-gradient(135deg,var(--accent),#0b7d86);color:#fff}"
        ".btn-ghost{background:#fff;border:1px solid var(--line);color:var(--muted)}"
        "button:hover{filter:brightness(1.03)}button:active{transform:translateY(1px)}"
        ".hint{margin-top:10px;color:var(--muted);font-size:13px;line-height:1.5}"
        "@media (max-width:520px){.hero h1{font-size:24px}.hero p{font-size:14px}}"
        "</style></head><body><section class='panel'>"
        "<header class='hero'><h1>AI Robot Wi-Fi Setup</h1>"
        "<p>Connect your robot to home Wi-Fi. Choose a scanned network or enter SSID manually.</p>"
        "</header><form action='/show' method='POST' enctype='text/plain'>"
        "<div class='grid'>"
        "<div><label for='wifi_list'>Scanned Networks</label><select id='wifi_list'></select></div>"
        "<div><label for='wifi_username'>SSID</label><input id='wifi_username' name='wifi_username'"
        " placeholder='Enter Wi-Fi name' required></div>"
        "<div><label for='wifi_password'>Password</label><input id='wifi_password' name='wifi_password'"
        " type='password' placeholder='Leave blank for open network'></div>"
        "</div><div class='actions'>"
        "<button class='btn-main' type='submit'>Save & Reconnect</button>"
        "<button class='btn-ghost' type='button' id='copy_selected'>Use Selected Network</button>"
        "</div><p class='hint'>After saving, the device will restart and switch to STA mode.</p>"
        "</form><script>const wifiNetworks=[";

    const char *tail =
        "];const s=document.getElementById('wifi_list');"
        "const u=document.getElementById('wifi_username');"
        "const c=document.getElementById('copy_selected');"
        "wifiNetworks.forEach(w=>{const o=document.createElement('option');o.value=w;o.textContent=w;s.appendChild(o);});"
        "const sync=()=>{if(s.value){u.value=s.value;}};"
        "s.addEventListener('change',sync);"
        "c.addEventListener('click',sync);"
        "if(s.options.length>0){s.selectedIndex=0;sync();}"
        "</script></section></body></html>";

    size_t list_len = wifiList ? strlen(wifiList) : 0;
    size_t total = strlen(head) + list_len + strlen(tail) + 1;
    char *html = (char *)malloc(total);
    if (html == NULL) {
        return NULL;
    }

    strcpy(html, head);
    if (wifiList) {
        strcat(html, wifiList);
    }
    strcat(html, tail);
    return html;
}
