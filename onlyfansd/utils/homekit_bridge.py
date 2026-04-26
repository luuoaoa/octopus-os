import logging
import threading

from pyhap.accessory import Accessory, Bridge
from pyhap.accessory_driver import AccessoryDriver
from pyhap.const import CATEGORY_FAN


class CeilingFanAccessory(Accessory):
    category = CATEGORY_FAN

    def __init__(self, driver, name, *, room, on_fan_command, on_light_command):
        super().__init__(driver, name)
        self.room = room
        self.on_fan_command = on_fan_command
        self.on_light_command = on_light_command
        self._light_on = False

        fan_service = self.add_preload_service('Fan', chars=['RotationSpeed', 'RotationDirection'])
        self.char_active = fan_service.configure_char(
            'On', setter_callback=self._set_active)
        self.char_speed = fan_service.configure_char(
            'RotationSpeed', setter_callback=self._set_speed)
        self.char_direction = fan_service.configure_char(
            'RotationDirection', setter_callback=self._set_direction)

        light_service = self.add_preload_service('Lightbulb')
        self.char_light_on = light_service.configure_char(
            'On', setter_callback=self._set_light)

    def _set_active(self, value):
        if value:
            self.on_fan_command(self.room, 'turn_on', {'percentage': 35})
        else:
            self.on_fan_command(self.room, 'turn_off', {})

    def _set_speed(self, value):
        if value == 0:
            self.on_fan_command(self.room, 'turn_off', {})
        else:
            self.on_fan_command(self.room, 'set_percentage', {'percentage': value})

    def _set_direction(self, value):
        # 0 = clockwise (forward), 1 = counter-clockwise (reverse)
        direction = 'reverse' if value == 1 else 'forward'
        self.on_fan_command(self.room, 'set_direction', {'direction': direction})

    def _set_light(self, value):
        # Only send toggle if the desired state differs from current
        if value != self._light_on:
            self.on_light_command(self.room, 'toggle', {})

    def update_fan(self, state):
        self.char_active.set_value(state['state'] == 'ON')
        self.char_speed.set_value(state.get('percentage', 0))
        direction_value = 1 if state.get('direction', 'forward') == 'reverse' else 0
        self.char_direction.set_value(direction_value)

    def update_light(self, state):
        on = state['state'] == 'ON'
        self._light_on = on
        self.char_light_on.set_value(on)


class HomeKitBridge:
    def __init__(self, fans, port, persist_file, on_fan_command, on_light_command):
        self.driver = AccessoryDriver(port=port, persist_file=persist_file)
        bridge = Bridge(self.driver, 'Ceiling Fans')

        self.accessories = {}
        for room in fans:
            name = room.replace('_', ' ').title() + ' Fan'
            acc = CeilingFanAccessory(
                self.driver, name,
                room=room,
                on_fan_command=on_fan_command,
                on_light_command=on_light_command,
            )
            bridge.add_accessory(acc)
            self.accessories[room] = acc

        self.driver.add_accessory(accessory=bridge)

    def update_fan(self, room, state):
        if room in self.accessories:
            self.accessories[room].update_fan(state)

    def update_light(self, room, state):
        if room in self.accessories:
            self.accessories[room].update_light(state)

    def start(self):
        t = threading.Thread(target=self.driver.start, daemon=True)
        t.start()
        logging.info("HomeKit bridge started")
