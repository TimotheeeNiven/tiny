import argparse
import os
import time
import yaml
import numpy as np
from sklearn.metrics import roc_auc_score

from datasets import DataSet
from device_manager import DeviceManager
from device_under_test import DUT
from script import Script

"""
Application to execute test scripts to measure power consumption, turn on and off power, send commands to a device
under test.
"""

def init_dut(device):
    if device:
        with device as dut:
            time.sleep(2)
            dut.get_name()
            dut.get_model()
            dut.get_profile()

def identify_dut(manager):
    interface = manager.get("interface", {}).get("instance")
    power = manager.get("power", {}).get("instance")
    if not manager.get("dut") and interface: # removed and power:
        dut = DUT(interface, power_manager=power)
        manager["dut"] = {
            "instance": dut
        }
    else:
        dut = manager.get("dut", {}).get("instance")
    init_dut(dut)


def run_test(devices_config, dut_config, test_script, dataset_path, mode):
    """Run the test

    :param devices_config:
    :param dut_config:
    :param test_script:
    :param dataset_path:
    :param mode: Test mode (energy, performance, accuracy)
    """
    manager = DeviceManager(devices_config)
    manager.scan()
    power = manager.get("power", {}).get("instance")
    print(f"Power instance: {power}")
    
    if power and dut_config and dut_config.get("voltage"):
        power.configure_voltage(dut_config["voltage"])
    identify_dut(manager)  # hangs in identify_dut()=>init_dut()=>time.sleep()

    dut = manager.get("dut", {}).get("instance")
    io = manager.get("interface", {}).get("instance")

    # Create a Script object from the dict that was read from the tests yaml file.
    script = Script(test_script.get(dut.get_model()))

    # Run the test script with the mode passed in
    data_set = DataSet(os.path.join(dataset_path, script.model), script.truth)
    return script.run(io, dut, data_set, mode)  # Pass mode to the run method


def parse_device_config(device_list_file, device_yaml):
    """Parsee the device discovery configuration

    :param device_list: device discovery configuration file
    :param device_yaml: device description as raw yaml
    """
    if device_yaml:
        return yaml.load(device_yaml)
    else:
        with open(device_list_file) as dev_file:
            return yaml.load(dev_file, Loader=yaml.CLoader)


def parse_dut_config(dut_cfg_file, dut_voltage, dut_baud):
    """ Parse the dut configuration file and override values

    :param dut: path to device config file
    :param dut_voltage: dut voltage in mV
    :param dut_baud: dut baud rate
    """
    config = {}
    if dut_cfg_file:
        with open(dut_cfg_file) as dut_file:
            dut_config = yaml.load(dut_file, Loader=yaml.CLoader)
            config.update(**dut_config)
    if dut_voltage:
        config.update(voltage=dut_voltage)
    if dut_baud:
        config.update(baud=dut_baud)
    return config


def parse_test_script(test_script):
    """Load the test script

    :param test_script: The path to the test script definition
    """
    with open(test_script) as test_file:
        return yaml.load(test_file, Loader=yaml.CLoader)

def normalize_probabilities(probabilities):
    """Normalize probabilities to ensure they sum to 1.0, no matter the initial sum."""
    probabilities = np.array(probabilities)
    sum_probs = np.sum(probabilities)
    
    # Ensure probabilities sum exactly to 1
    if sum_probs == 0:
        raise ValueError("Sum of probabilities is zero, cannot normalize.")
    
    probabilities = probabilities / sum_probs  # Normalize probabilities to make sum = 1
    
    return probabilities

from collections import Counter
import numpy as np
from sklearn.metrics import roc_auc_score

def summarize_result(result):
    """
    Summarizes results based on mode:
    - 'a' : Accuracy and AUC calculations
    - 'p' : Performance metrics like runtime and throughput
    - 'e' : Reserved for energy calculations (to be implemented)
    """
    # Initialize counters and storage for results
    num_correct_files = 0  # Correct files (not individual predictions)
    total_files = 0  # Total number of files processed
    true_labels = []
    predicted_probabilities = []
    file_names = []

    # Create a dictionary to aggregate results by file name
    file_infer_results = {}

    for r in result:
        infer_results = r['infer']['results']
        file_name = r['file']
        true_class = int(r['class'])

        if file_name not in file_infer_results:
            file_infer_results[file_name] = {'true_class': true_class, 'results': []}
        
        # If there is only one inference result, we'll collect them for majority voting
        if len(infer_results) == 1:
            class_label = 0 if infer_results[0] > 10 else 1
            infer_results = [class_label, 1 - class_label]  # Two-class probability setup
            # Add to the dictionary under the file name key
            file_infer_results[file_name]['results'].append(infer_results)
        else:
            infer_results = normalize_probabilities(infer_results)
            # Add to the list for AUC calculation
            file_infer_results[file_name]['results'].append(infer_results)

    # Process the aggregated results and determine the class for each file
    for file_name, data in file_infer_results.items():
        true_class = data['true_class']
        results = data['results']

        # Count occurrences of class predictions
        class_counts = Counter([np.argmax(res) for res in results])
        
        # Determine the majority class
        majority_class = class_counts.most_common(1)[0][0]  # Get the most frequent class
        
        # Increment num_correct_files if the majority class matches the true class
        if majority_class == true_class:
            num_correct_files += 1
        
        # Store true label and predicted probabilities for AUC calculation
        true_labels.append(true_class)
        predicted_probabilities.append(results[0])  # Use the first probability set for AUC

        # Increment total_files processed
        total_files += 1

    # Convert lists to numpy arrays for AUC calculation
    true_labels = np.array(true_labels)
    predicted_probabilities = np.array(predicted_probabilities)

    # Accuracy calculation based on files, not individual results
    accuracy = num_correct_files / total_files
    print(f"Accuracy = {num_correct_files}/{total_files} = {100*accuracy:4.2f}%")   

    # Check if binary classification (i.e., only two unique classes)
    if len(np.unique(true_labels)) == 2:
        # For binary classification, use the probability of class 1
        predicted_probabilities = predicted_probabilities[:, 1].reshape(-1, 1)  # Use class 1 probability

        # Compute AUC for binary classification (class 1)
        try:
            auc_score = roc_auc_score(true_labels, predicted_probabilities)
            print(f"AUC: {auc_score:.4f}")
        except ValueError as e:
            print(f"AUC calculation failed: {e}")
    else:
        # Multiclass AUC calculation
        try:
            # Use the one-vs-rest approach for AUC calculation
            auc_score = roc_auc_score(
                true_labels, 
                predicted_probabilities, 
                multi_class='ovr'
            )
            print(f"Multiclass AUC (One-vs-Rest): {auc_score:.4f}")
        except ValueError as e:
            print(f"Multiclass AUC calculation failed: {e}")

              
if __name__ == '__main__':
    parser = argparse.ArgumentParser(prog="TestRunner", description=__doc__)
    parser.add_argument("-d", "--device_list", default="devices.yaml", help="Device definition YAML file")
    parser.add_argument("-y", "--device_yaml", required=False, help="Raw YAML to interpret as the target device")
    parser.add_argument("-u", "--dut_config", required=False, help="Target device")
    parser.add_argument("-v", "--dut_voltage", required=False, help="Voltage set during test")
    parser.add_argument("-b", "--dut_baud", required=False, help="Baud rate for device under test")
    parser.add_argument("-t", "--test_script", default="tests.yaml", help="File containing test scripts")
    parser.add_argument("-s", "--dataset_path", default="datasets")
    parser.add_argument("-m", "--mode", choices=["e", "p", "a"], default="a", help="Test mode (energy (e), performance (p), accuracy (a))")
    args = parser.parse_args()
    config = {
        "devices_config": parse_device_config(args.device_list, args.device_yaml),
        "dut_config": parse_dut_config(args.dut_config, args.dut_voltage, args.dut_baud),
        "test_script": parse_test_script(args.test_script),
        "dataset_path": args.dataset_path,
        "mode": args.mode
    }
    result = run_test(**config)
    summarize_result(result)