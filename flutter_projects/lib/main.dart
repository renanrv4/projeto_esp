import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'dart:io';
import 'dart:convert';
import 'package:csv/csv.dart';

void main() => runApp(MyApp());

class MyApp extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: Text("ESP32 CAN Logger")), 
        body: UDSControl(),
      ),
    );
  }
}

class UDSControl extends StatefulWidget {
  @override
  _UDSControlState createState() => _UDSControlState();
}

class _UDSControlState extends State<UDSControl> {
  TextEditingController ipController = TextEditingController();
  TextEditingController ssidController = TextEditingController();
  TextEditingController passwordController = TextEditingController();
  List<String> udsRequests = [
    'sendDDoSECUHardwareVersion',
    'sendDDoSECUHardwareNumber',
    'sendDDoSECUSoftwareVersion',
    'sendDDoSECUSoftwareNumber'
  ];
  String selectedRequest = 'sendDDoSECUHardwareVersion';
  TextEditingController logController = TextEditingController();
  String requests = '';
  String descriptions = '';
  bool isRunning = false;
  bool showForm = false;

  // Adiciona dados ao arquivo CSV
  Future<void> addDataToCsv(List<String> newRow) async {
    try {
      final directory = await getApplicationDocumentsDirectory();
      final path = '${directory.path}/uds_log.csv';
      final file = File(path);

      List<List<dynamic>> csvData = [];

      if (await file.exists()) {
        final existingContent = await file.readAsString();
        if (existingContent.trim().isNotEmpty) {
          csvData = const CsvToListConverter().convert(existingContent);
        }
      }

      csvData.add(newRow);

      final csvContent = const ListToCsvConverter().convert(csvData);
      await file.writeAsString(csvContent);

      print('Data added to CSV: $newRow');

    } catch (e) {
      print('Error writing to CSV: $e');
    }
  }

  // Limpa o csv
  Future<void> clearCsv() async {
    try {
      final directory = await getApplicationDocumentsDirectory();
      final path = '${directory.path}/uds_log.csv';
      final file = File(path);

      if (await file.exists()) {
        await file.writeAsString(''); // Limpa o conteúdo do arquivo
        print('CSV file cleared');
      } else {
        print('CSV file does not exist');
      }
    } catch (e) {
      print('Error clearing CSV: $e');
    }
  }

  // Limpa o log na interface e no ESP32
  /*
  Future<void> clearLog(String ip) async {
    while(isRunning) {
      final log = await fetchStructuredLog(ip);
      if (log.isNotEmpty) {
        for (var entry in log) {
          await addDataToCsv([
            "${entry['timestamp'] ?? ''}",
            "${entry['channel'] ?? ''}",
            "${entry['canType'] ?? ''}",
            "${entry['frameType'] ?? ''}",
            "${entry['canId'] ?? ''}",
            "${entry['dlc'] ?? ''}",
            "${entry['data'] ?? ''}",
            "${entry['label'] ?? ''}",
          ]);
        }
      }
      var url = Uri.parse('http://$ip/clear_log');
      var response = await http.get(url);
      if (response.statusCode == 200) {
        setState(() {
          logController.text = ""; requests = ""; descriptions = "";
        });
      }
      await Future.delayed(Duration(seconds: 10)); // Aguarda 5 segundos antes de tentar limpar novamente
    }
  }*/

  // Envio de requisições UDS
  Future<void> startUDSRequests(String ip) async {
    var url = Uri.parse('http://$ip/start');
    var response = await http.get(url);
    if (response.statusCode == 200) {
      setState(() {
        isRunning = true;
      });
      fetchLog(ip);
      // clearLog(ip);    
    }
  }

  // Envio direto de uma requisição UDS
  Future<void> sendUDSRequest(String ip, String request) async {
    /*
    var url = Uri.parse('http://$ip/send?request=$request');
    var response = await http.get(url);
    if (response.statusCode == 200) {
      setState(() {
        // Para não sobrecarregar esse métodos só realiza o request sem o print
        // logController.text += "\n[Request Sent]: $request";  
      });
    }
    */
  }

  // Parar envio de requisições UDS
  Future<void> stopUDSRequests(String ip) async {
    var url = Uri.parse('http://$ip/stop');
    var response = await http.get(url);
    if (response.statusCode == 200) {
      setState(() {
        isRunning = false;
      });
    }
  }

  // Atualiza o estado atual do log
  Future<void> fetchLog(String ip) async {
    while (isRunning) {
      var url = Uri.parse('http://$ip/log');
      var response = await http.get(url);
      if (response.statusCode == 200) {
        setState(() {
          logController.text = response.body;
        });
        // Check if log contains "50 3 XX XX"
        RegExp pattern = RegExp(r'50 3 (\S+) (\S+)');
        Match? match = pattern.firstMatch(response.body);
        if (match != null) {
          String byte1 = match.group(1)!;
          String byte2 = match.group(2)!;
          callServerFunction('$byte1 $byte2');
        }
      }
      await Future.delayed(Duration(seconds: 1)); // Atualiza o log a cada 1 segundo
    }
  }

  // Atualiza o estado atual do log estruturado para envio ao Firebase
  Future<List<Map<String, dynamic>>> fetchStructuredLog(String ip) async {
    final url = Uri.parse('http://$ip/structured_log');
    final response = await http.get(url);

    if (response.statusCode == 200) {
      try {
        final List<dynamic> jsonData = json.decode(response.body);
        return List<Map<String, dynamic>>.from(jsonData);
      } catch (e) {
        print("Erro ao decodificar JSON do structured_log: $e");
        return [];
      }
    } else {
      print("Erro ao buscar structured_log: ${response.statusCode}");
      return [];
    }
  }

  // Função que faz a chamada ao servidor quando a condição é atendida.
  Future<void> callServerFunction(String extractedData) async {
    List<String> bytes = extractedData.split(" ");
    if (bytes.length < 2) return;

    String byte1 = bytes[0];
    String byte2 = bytes[1];

    var url = Uri.parse(
        'http://192.168.10.6:5138/dll/execute_function?data1=$byte1&data2=$byte2');
    var response = await http.get(url);

    if (response.statusCode == 200) {
      setState(() {
        logController.text += "\n[DLL Response]: ${response.body}";
      });
    }
  }

  // Envia o log estruturado para o Firebase
  Future<void> sendLogToFirebase(List<Map<String, dynamic>> logEntries) async {
    final url = Uri.parse('https://espcan-8e413-default-rtdb.firebaseio.com/logs.json');    
    final response = await http.post(
      url,
      headers: {"Content-Type": "application/json"},
      body: json.encode({
        'timestamp': DateTime.now().toIso8601String(),
        'logEntries': logEntries,
      }),
    );

    if(response.statusCode == 200) {
      print("Log enviado com sucesso para o Firebase");
    } else {
      print("Erro ao enviar log para o Firebase: ${response.statusCode}");
    }
  }

  Future<void> sendCsvToFirebase() async {
    try {
      final directory = await getApplicationDocumentsDirectory();
      final path = '${directory.path}/uds_log.csv';
      final file = File(path);

      if (!await file.exists()) {
        print("CSV não encontrado em: $path");
        return;
      }

      final csvContent = await file.readAsString();
      final rows = const CsvToListConverter().convert(csvContent);

      // Ordem das colunas esperada
      final headers = [
        "timestamp",
        "channel",
        "canType",
        "frameType",
        "canId",
        "dlc",
        "data",
        "label"
      ];

      // Transforma cada linha em Map<String, dynamic>
      final List<Map<String, dynamic>> logEntries = rows.map((row) {
        final Map<String, dynamic> entry = {};
        for (int i = 0; i < headers.length && i < row.length; i++) {
          if (headers[i] == "canId") {
            // força para int e converte para hexadecimal no formato 0x
            final int id = row[i] is num ? row[i].toInt() : int.tryParse(row[i].toString()) ?? 0;
            entry["canId"] = "0x${id.toRadixString(16).toUpperCase()}";
          } else {
            entry[headers[i]] = row[i].toString();
          }
        }
        return entry;
      }).toList();

      // Agora envia pro Firebase
      await sendLogToFirebase(logEntries);
    } catch (e) {
      print("Erro ao enviar CSV para Firebase: $e");
    }
  }

  //---------------------------------------
  // UI do APP
  //---------------------------------------
  
  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(16.0),
      child: Column(
        children: [
          TextField(
            controller: ipController,
            decoration: InputDecoration(labelText: "ESP32 IP Address"),
          ),
          Row(
            children: [
              ElevatedButton(
                onPressed: isRunning
                    ? null
                    : () => startUDSRequests(ipController.text),
                child: Text("Start"),
              ),
              SizedBox(width: 10),
              ElevatedButton(
                onPressed: !isRunning
                    ? null
                    : () => stopUDSRequests(ipController.text),
                child: Text("Stop"),
              ),
            ],
          ),
          SizedBox(height: 10),
          Align(
            alignment: Alignment.centerLeft,
            child: DropdownButton<String>(
              value: selectedRequest,
              onChanged: (String? newValue) {
                setState(() {
                  selectedRequest = newValue!;
                });
              },
              items: udsRequests.map<DropdownMenuItem<String>>((String value) {
                return DropdownMenuItem<String>(
                  value: value,
                  child: Text(value),
                );
              }).toList(),
            ),
          ),
          Row(
            children: [
              ElevatedButton(
                onPressed: () {
                  sendUDSRequest(ipController.text, selectedRequest);
                },
                child: Text("Send Request"),
              ),
            ],
          ),
          SizedBox(height: 10),
          Expanded(
            child: SingleChildScrollView(
              child: TextField(
                controller: logController,
                maxLines: null,
                decoration: InputDecoration(labelText: "Log"),
                readOnly: true,
              ),
            ),
          ),
          Row (
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              ElevatedButton(
                onPressed: () {
                  // clearLog(ipController.text);
                },
                child: Text("Clear Log"),
              ),
              SizedBox(width: 10),
              ElevatedButton(
                onPressed: () async {
                  try {
                    await sendCsvToFirebase();
                    ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(content: Text("Log salvo com sucesso"))
                      );
                    await clearCsv();
                    stopUDSRequests(ipController.text);
                  } catch (e) {
                    ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(content: Text("Erro ao salvar log: $e"))
                      );
                  }
                },
                child: Text("Salvar Log"),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
