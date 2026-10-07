#pragma once

// nr-bridge [NRB6]: see nrb_dxgi_unhide.cpp. Call before NGX is initialised.
// Idempotent; `logger` receives one line per decision. Returns true when installed.
namespace nrb
{
    bool install_dxgi_unhide(void (*logger)(const char *));
}
