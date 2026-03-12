# Release Checklist (Device-development)

## 1. Communication sanity
- [ ] PLC MC mode read OK
- [ ] PLC Modbus mode read OK
- [ ] INV Mitsubishi clink read OK
- [ ] INV Modbus read OK

## 2. UI sanity
- [ ] PLC page transition works
- [ ] Word/bit rendering correct (16/32bit)
- [ ] Save & Apply persists after reboot

## 3. Logging
- [ ] SD CSV file created
- [ ] CSV append works continuously
- [ ] RTC timestamp correct

## 4. Stability
- [ ] 30-min continuous run no freeze
- [ ] 12-hour soak test no watchdog reset

## 5. Packaging
- [ ] README updated
- [ ] CHANGELOG updated
- [ ] Version/tag assigned
