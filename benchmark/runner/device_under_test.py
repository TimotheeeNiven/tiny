import re
import sys

from interface_device import InterfaceDevice
from serial_device import SerialDevice


class DUT:
  def __init__(self, port_device, baud_rate=115200, power_manager=None):
    interface = port_device
    if not isinstance(port_device, InterfaceDevice):
      interface = SerialDevice(port_device, baud_rate, "m-ready", '%')
    self._port = interface
    self.power_manager = power_manager
    self._profile = None
    self._model = None
    self._name = None
    self._max_bytes = 26 if power_manager else 31

    def __enter__(self):
        if self.power_manager:
            self.power_manager.__enter__()
        self._port.__enter__()
        return self

    def __exit__(self, *args):
        self._port.__exit__(*args)
        if self.power_manager:
            self.power_manager.__exit__(*args)

    def _get_name(self):
        name_retrieved = False
        for l in self._port.send_command("name"):
            match = re.match(r'^m-(name)-dut-\[([^]]+)]$', l)
            if match:
                self.__setattr__(f"_{match.group(1)}", match.group(2))
                name_retrieved = True
                print(f"Name found: {self._name}")
        if not name_retrieved:
            print(f"WARNING: Failed to get name.")
  def _get_name(self):
    for l in self._port.send_command("name"):
      match = re.match(r'^m-(name)-dut-\[([^]]+)]$', l)
      if match:
        self.__setattr__(f"_{match.group(1)}", match.group(2))

    def get_name(self):
        if self._name is None:
            self._get_name()
        else:
            print(f"Name: {self._name}")
        return self._name

    def _get_profile(self):
        for l in self._port.send_command("profile"):
            match = re.match(r'^m-(model|profile)-\[([^]]+)]$', l)
            if match:
                self.__setattr__(f"_{match.group(1)}", match.group(2))
                print(f"{match.group(1).capitalize()} found: {match.group(2)}")

    def get_model(self):
        if self._model is None:
            self._get_profile()
        else:
            print(f"Model: {self._model}")
        return self._model

    def get_profile(self):
        if self._profile is None:
            self._get_profile()
        else:
            print(f"Profile: {self._profile}")
        return self._profile

    def timestamp(self):
        return self._port.send_command("timestamp")

    def send_data(self, data):
        size = len(data)
        pass

    def load(self, data):
        self._port.send_command(f"db load {len(data)}")
        i = 0
        while i < len(data):
            cmd = f"db {''.join(f'{d:02x}' for d in data[i:i+self._max_bytes])}"
            result = self._port.send_command(cmd)
            i += self._max_bytes
        return result

    def infer(self, number, warmups):
        result = self._port.send_command("db print")

        command = f"infer {number} {warmups}"  # must include warmups, even if 0, because default warmups=10
        if self.power_manager:
            print(self.power_manager.start())
        result = self._port.send_command(command)
        if self.power_manager:
            print(self.power_manager.stop())
        return result
  def infer(self, number, warmups):
    command = f"infer {number}"
    if warmups:
      command += f" {warmups}"
    if self.power_manager:
      print(self.power_manager.start())
    result = self._port.send_command(command)
    if self.power_manager:
      print(self.power_manager.stop())
    return result

    def get_help(self):
        return self._port.send_command("help")
