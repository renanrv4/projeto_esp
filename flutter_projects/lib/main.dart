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
  String? idToken;
  bool isRunning = false;
  bool showForm = false;

  Future<void> createLogSession(String ip) async {
    final url = Uri.parse(
      'https://espcan-8e413-default-rtdb.firebaseio.com/logs.json'
    );

    final response = await http.post(
      url,
      headers: {"Content-Type": "application/json"},
      body: json.encode({
        "startedAt": DateTime.now().toIso8601String(),
      }),
    );
    if (response.statusCode == 200) {
      final data = json.decode(response.body);
      idToken = data["name"]; // chave gerada pelo Firebase
      print("Sessão criada: $idToken");
    } else {
      throw Exception("Erro ao criar sessão");
    }
  }

  // Envio de requisições UDS
  Future<void> startUDSRequests(String ip) async {
    await createLogSession(ip);
    var url = Uri.parse('http://$ip/start');
    var response = await http.get(url);
    if (response.statusCode == 200) {
      setState(() {
        isRunning = true;
      });
      fetchStructuredLog(ip);
      // clearLog(ip);
    }
  }

  Future<void> pushLogEntry(Map<String, dynamic> entry) async {
    if(!isRunning || idToken == null) return;

    final url = Uri.parse('https://espcan-8e413-default-rtdb.firebaseio.com/logs/$idToken/entries.json');
    await http.post(
      url,
      headers: {"Content-Type": "application/json"},
      body: json.encode(entry),
    );
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
  Future<void> fetchStructuredLog(String ip) async {
    while (isRunning) {
      final url = Uri.parse('http://$ip/structured_log');
      final response = await http.get(url);

      if (response.statusCode == 200) {
        final decoded = json.decode(response.body);
        final List logs = decoded['logs'];

        for (final log in logs) {
          await pushLogEntry(log);
        }

        if (logs.isNotEmpty) {
          await http.post(
            Uri.parse('http://$ip/structured_log/ack'),
            headers: {"Content-Type": "application/json"},
            body: json.encode({"count": logs.length}),
          );
        }
      }

      await Future.delayed(const Duration(milliseconds: 200));
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
            ],
          ),
        ],
      ),
    );
  }
}
