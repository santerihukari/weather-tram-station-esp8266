# Tram + Weather OLED Display

Displays local time, date, weather, and the next public transport departures on an I2C OLED.

## Main functionality
- Connect to WiFi
- Sync local time via NTP
- Fetch weather from the **Open-Meteo API**
- Fetch upcoming departures from the **Digitransit GraphQL API**
- Render time, date, weather icon, temperature, and next departures on an I2C display
- Refresh clock frequently and network data periodically

## APIs used
Weather  
https://api.open-meteo.com

Transit departures  
https://api.digitransit.fi (GraphQL endpoint)

A **Digitransit API subscription key must be created** and included in requests.

## Generic LLM generation spec
An LLM can reproduce the same functionality for any microcontroller that has:
- WiFi networking
- HTTPS client capability
- I2C support
- a graphical display driver (OLED/LCD)
- JSON parsing support
- NTP or equivalent time synchronization

The generated firmware should:
1. initialize WiFi, I2C, display, and time synchronization
2. periodically fetch weather data from **Open-Meteo**
3. periodically fetch public transport departures from **Digitransit** (using an API key)
4. parse JSON responses
5. maintain the next few departures sorted by time
6. render a compact UI containing time, date, weather, temperature, and departures

## Porting
To adapt to another board, typically only these layers change:
- WiFi library
- HTTPS client
- display driver
- I2C pin configuration
- timezone/time API
