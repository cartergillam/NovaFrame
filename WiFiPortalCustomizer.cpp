#include "WiFiPortalCustomizer.h"

// Fully style the portal with NovaFrame branding
void setupCustomWiFiManager(WiFiManager &wm) {
  // Minimal menu: just Wi-Fi and Exit
  std::vector<const char*> menu = {"wifi", "exit"};
  wm.setMenu(menu);

  // Optional: Set page title and favicon (empty)
  wm.setTitle("NovaFrame Setup");
  wm.setCustomHeadElement("<link rel=\"icon\" href=\"data:,\">");

  // Custom CSS + welcome HTML
  wm.setCustomMenuHTML(R"rawliteral(
    <style>
      body {
        background-color: #111;
        color: #fff;
        font-family: 'Segoe UI', sans-serif;
        text-align: center;
        padding-top: 40px;
        margin: 0;
      }
      h1 {
        color: #00d0ff;
        margin-bottom: 10px;
      }
      p {
        color: #ccc;
        margin-top: 0;
        padding: 0 20px;
        font-size: 16px;
      }
      input, button {
        padding: 10px;
        font-size: 16px;
        border-radius: 6px;
        border: none;
        margin: 12px;
        width: 85%;
        max-width: 320px;
        display: block;
        margin-left: auto;
        margin-right: auto;
      }
      button {
        background-color: #00d0ff;
        color: #fff;
        font-weight: bold;
        cursor: pointer;
      }
      button:hover {
        background-color: #00b8e0;
      }
    </style>
    <h1>NovaFrame Setup</h1>
    <p>Select your Wi-Fi network below, enter the password, and hit Save.<br>
    Once connected, you can close this page.</p>
  )rawliteral");
}