/********************************************************
 * Copyright [2024] Tobias Faust <tobias.faust@gmx.net 
 ********************************************************/

#include "modbus.h"

/*******************************************************
 * Constructor
*******************************************************/
modbus::modbus(): enableRelays(false), 
                  Baudrate(19200), 
                  enableCrcCheck(true), 
                  enableLengthCheck(true), 
                  LastTxLiveData(0), 
                  LastTxIdData(0), 
                  LastTxInverter(0),
                  Conf_OpenWBModulID(1),
                  Conf_OpenWBBatteryID(2) { 
  
  DataFrame           = new std::vector<byte>{};
  SaveIdDataframe     = new std::vector<byte>{};
  SaveLiveDataframe   = new std::vector<byte>{};

  InverterLiveData    = new std::vector<reg_t>{};
  InverterIdData      = new std::vector<reg_t>{};
  AvailableInverters  = new std::vector<regfiles_t>{};
  Setters             = new std::vector<setter_t>{};
  OpenWB              = new openwb();

  Conf_RequestLiveData= new std::vector<std::vector<byte>>{};
  Conf_RequestIdData  = new std::vector<std::vector<byte>>{};

  InverterType        = {};

  ReadQueue = new ArduinoQueue<std::vector<byte>>(5); // max 5 read requests parallel
  SetQueue  = new ArduinoQueue<std::vector<byte>>(5); // max 5 set requests parallel

  if (Config->GetUseETH()) {
    this->pin_RX = this->default_pin_RX = 2;
    this->pin_TX = this->default_pin_TX = 4;
    this->pin_RTS = this->default_pin_RTS = 5;
    this->pin_Relay1 = this->default_pin_Relay1 = 12;
    this->pin_Relay2 = this->default_pin_Relay2 = 14;
  } else {
    this->pin_RX = this->default_pin_RX = 16;
    this->pin_TX = this->default_pin_TX = 17;
    this->pin_RTS = this->default_pin_RTS = 5;
    this->pin_Relay1 = this->default_pin_Relay1 = 18;
    this->pin_Relay2 = this->default_pin_Relay2 = 19;
  }

  this->LoadInvertersFromJson(); //needed for selecting default inverter in LoadJsonConfig
  this->LoadJsonConfig(true);
  this->OpenWB->begin(this->Conf_OpenWBVersion);
  this->init(true);
}

/*******************************************************
 * initialize transmission
*******************************************************/
void modbus::init(bool firstrun) {
  Config->logN(3, "Start Hardwareserial 1 on RX (%d), TX(%d), RTS(%d)", this->pin_RX, this->pin_TX, this->pin_RTS);
  Config->logN(3, "Init Modbus to Client 0x%02X with %d Baud", this->ClientID, this->Baudrate);

  // Configure Direction Control pin
  pinMode(this->pin_RTS, OUTPUT);  
  if (this->enableRelays) {
    pinMode(this->pin_Relay1, INPUT_PULLUP);
    pinMode(this->pin_Relay2, INPUT_PULLUP);
  }

  this->LoadInverterConfigFromJson();
  this->LoadRegItems(this->InverterIdData, "id"); // load item definitions from register file
  this->LoadRegItems(this->InverterLiveData, "livedata"); // load item definitions from register file
  this->LoadSettersFromRegFile(); // loads setter config from config file and updates Setters
  this->LoadJsonItemConfig(); // loads item config (active/inactive) from config file and updates InverterIdData, InverterLiveData and Setters
  

  // https://forum.arduino.cc/t/creating-serial-objects-within-a-library/697780/11
  RS485Serial = new HardwareSerial(1);
  RS485Serial->begin(this->Baudrate, SERIAL_8N1, this->pin_RX, this->pin_TX);

  //at first read ID Data
  this->QueryIdData();
}

/*******************************************************
* set websocket callback
********************************************************/
void modbus::setWebSocketCallback(std::function<void(String&)> callback) {
    webSocketCallback = callback;
}

/*******************************************************
 * Read configured pin states
*******************************************************/
void modbus::ReadRelays() {
  this->state_Relay1 = digitalRead(this->pin_Relay1);    
  this->mqtt->Publish_Int("relay1", this->state_Relay1, false);
  
  this->state_Relay2 = digitalRead(this->pin_Relay2);    
  this->mqtt->Publish_Int("relay2", this->state_Relay2, false);

  if (webSocketCallback) {
    String message = "{\"data-id\": {\"relay1.value\":\"" + String(this->state_Relay1?"On":"Off") + "\",\"relay2.value\":\"" + String(this->state_Relay2?"On":"Off") + "\"}}";
    webSocketCallback(message);
  }
}

/*******************************************************
 * generate the full mqtt topic without /# at end
*******************************************************/
String modbus::GetMqttSetTopic(String command) {
  char s[100] = {0}; 
  memset(s, 0, sizeof(s));

  snprintf(s, sizeof(s), "%s/%s/set/%s", Config->GetMqttBasePath().c_str(), Config->GetMqttRoot().c_str(), command.c_str());
  return (String)s;
}

/*******************************************************
 * load all "set" register from regfile if setters are enabled globally
*******************************************************/
void modbus::LoadSettersFromRegFile() {
  // clear vector
  this->Setters->clear();
  
  File regfile = LittleFS.open("/regs/"+this->InverterType.filename);
  
  String streamString = "";
  streamString = "\""+ this->InverterType.name +"\": {";
  regfile.find(streamString.c_str());
  streamString = "\"set\": [";
  regfile.find(streamString.c_str());

  do {
    JsonDocument elem;
    DeserializationError error = deserializeJson(elem, regfile); 
    if (!error) {
      // Print the result
      Config->logN(4, "parsing JSON for <set> data ok");
      Config->log(5, elem);
     
      if(!elem["name"].isNull() && elem["request"].is<JsonArray>()) {
        setter_t s = {};
        s.Name = elem["name"].as<String>();

        Config->logN(4, "Set command successfully parsed from JSON: %s", s.Name.c_str());
        this->Setters->push_back(s);

      } else {
        continue;
      }
     
    } else {
      Config->logN(1, "Failed to parse JSON Register <set> Data: %s", error.c_str());
    }
  } while (regfile.findUntil(",","]"));

  regfile.close();
}

/*******************************************************
 * act on received mqtt command
*******************************************************/
void modbus::ReceiveMQTT(String topic, String msg) {
  if (!this->Conf_EnableSetters) {
    Config->logN(2, "Set command <%s> received, but setters over mqtt are currently disabled globally", topic.c_str());
    return;
  }

  for (uint8_t i = 0; i < this->Setters->size(); i++ ) {
    if (topic == this->GetMqttSetTopic(this->Setters->at(i).Name)) {
      if (!this->Setters->at(i).active) {
        Config->logN(2, "Set command <%s> received, but setter %s is not active", topic.c_str(), this->Setters->at(i).Name.c_str());
        return;
      }

      JsonDocument elem = this->GetSetterByName(this->Setters->at(i).Name);
      if (elem.isNull()) {
        Config->logN(1, "Setter %s not found in JSON", this->Setters->at(i).Name.c_str());
        return;
      }

      JsonArray arr = elem["request"].as<JsonArray>();
      std::vector<byte> request = {};

      for (String x : arr) {
        byte e = this->String2Byte(x);
        request.push_back(e);
      }

      // map values if a mapping is specified
      if(!elem["mapping"].isNull() && elem["mapping"].is<JsonArray>() && msg != "") {
        Config->logN(4, "Map values for item %s", msg.c_str());

        JsonArray map = elem["mapping"].as<JsonArray>();
        msg = this->MapItem(map, msg);
      }

      int msgInt = msg.toInt(); // atoi(msg.c_str())
      byte bytes[4];

      bytes[0] = (msgInt >> 24) & 0xFF;
      bytes[1] = (msgInt >> 16) & 0xFF;
      bytes[2] = (msgInt >> 8) & 0xFF;
      bytes[3] = (msgInt >> 0) & 0xFF;

      // 32bit number
      request.push_back(bytes[2]);
      request.push_back(bytes[3]);

      Config->logN(3, "MQTT Setter found: %s" ,this->Setters->at(i).Name.c_str());
      Config->logN(3, "Initiate Set Request to queue: %s" ,(this->PrintDataFrame(&request)).c_str());

      this->SetQueue->enqueue(request);
    }
  }
}

/*******************************************************
 * @brief get setter by name, read json file and return the json object for the setter
 * @param name: name of the setter
 * @return JsonDocument: json object for the setter
 * ******************************************************/
JsonDocument modbus::GetSetterByName(String name) {
  File regfile = LittleFS.open("/regs/" + this->InverterType.filename);
  if (!regfile) {
    Config->logN(1, "failed to open %s file", this->InverterType.filename.c_str());
    return JsonDocument();
  }

  String streamString = "";
  streamString = "\""+ this->InverterType.name +"\": {";
  regfile.find(streamString.c_str());
    
  streamString = "\"set\": [";
  regfile.find(streamString.c_str());
  do {
    JsonDocument elem;
    DeserializationError error = deserializeJson(elem, regfile); 
      
    if (!error) {
      // Print the result
      Config->logN(4, "parsing JSON ok");
      Config->log(5, elem);
    } else {
      Config->logN(1, "(Function GetSetterByName) Failed to parse JSON Register Data: %s", error.c_str()); 
    }

    if (elem["name"] == name) {
      regfile.close();
      return elem;
    }

  } while (regfile.findUntil(",","]"));

  if (regfile) { regfile.close(); }

  return JsonDocument();
}

/*******************************************************
 * get all defined inverters from json (register.h)
*******************************************************/
void modbus::LoadInvertersFromJson() {
  JsonDocument regjson;
  JsonDocument filter;

  AvailableInverters->clear();

  filter["*"]["config"]["ClientIdPos"] = true;  
  File root = LittleFS.open("/regs/");
  File file = root.openNextFile();
  while(file){
    Config->logN(3, "open register file from Filesystem: %s", file.name());

    DeserializationError error = deserializeJson(regjson, file, DeserializationOption::Filter(filter));
     if (!error && regjson.size() > 0) {
      // https://arduinojson.org/v6/api/jsonobject/begin_end/
      JsonObject root = regjson.as<JsonObject>();
      for (JsonPair kv : root) {
        Config->logN(3, "Inverter found: %s", kv.key().c_str());

        regfiles_t wr = {};
        wr.filename = file.name();
        wr.name = kv.key().c_str();
        AvailableInverters->push_back(wr);
      }
    } else{
      Config->logN(1, "Error: unable to load inverters from File %s: %s", file.name(), error.c_str());
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();

  if (this->AvailableInverters->size() == 0) {
    Config->logN(1, "ALERT: No register definitions found. ESP cannot work properly");
    Config->logN(1, "Please flash filesystem Image!");
  }
}

/*******************************************************
 * read config data from JSON, part "config"
*******************************************************/
void modbus::LoadInverterConfigFromJson() {
  JsonDocument doc;
  JsonDocument filter;

  File regfile = LittleFS.open("/regs/"+this->InverterType.filename);
  if (!regfile) {
    Config->logN(1, "failed to open %s file", this->InverterType.filename.c_str());
  }

  filter[this->InverterType.name]["config"] = true;
    
  DeserializationError error = deserializeJson(doc, regfile, DeserializationOption::Filter(filter));

  if (error) {
    Config->logN(1, "Error: unable to read configdata for inverter %s: %s", this->InverterType.name.c_str(), error.c_str());
  } else {
    Config->logN(4, "Read config data for inverter %s", this->InverterType.name.c_str());
    Config->log(4, doc);
  }

  //this->Conf_LiveDataFunctionCode   = this->String2Byte(doc[this->InverterType.name]["config"]["LiveDataFunctionCode"].as<String>());
  //this->Conf_IdDataFunctionCode     = this->String2Byte(doc[this->InverterType.name]["config"]["IdDataFunctionCode"].as<String>());
  this->Conf_LiveDataErrorCode      = this->String2Byte(doc[this->InverterType.name]["config"]["LiveDataErrorCode"].as<String>());
	this->Conf_IdDataErrorCode        = this->String2Byte(doc[this->InverterType.name]["config"]["IdDataErrorCode"].as<String>());
	//this->Conf_LiveDataSuccessCode    = this->String2Byte(doc[this->InverterType.name]["config"]["LiveDataSuccessCode"].as<String>());
	//this->Conf_IdDataSuccessCode      = this->String2Byte(doc[this->InverterType.name]["config"]["IdDataSuccessCode"].as<String>());
  this->Conf_ClientIdPos            = int(doc[this->InverterType.name]["config"]["ClientIdPos"]);
  //this->Conf_LiveDataStartsAtPos    = int(doc[this->InverterType.name]["config"]["LiveDataStartsAtPos"]);
	//this->Conf_IdDataStartsAtPos      = int(doc[this->InverterType.name]["config"]["IdDataStartsAtPos"]);
	this->Conf_LiveDataErrorPos       = int(doc[this->InverterType.name]["config"]["LiveDataErrorPos"]);
	this->Conf_IdDataErrorPos         = int(doc[this->InverterType.name]["config"]["IdDataErrorPos"]);
	//this->Conf_LiveDataSuccessPos     = int(doc[this->InverterType.name]["config"]["LiveDataSuccessPos"]);
	//this->Conf_IdDataSuccessPos       = int(doc[this->InverterType.name]["config"]["IdDataSuccessPos"]);
  //this->Conf_IdDataFunctionCodePos  = int(doc[this->InverterType.name]["config"]["IdDataFunctionCodePos"]);
  //this->Conf_LiveDataFunctionCodePos= int(doc[this->InverterType.name]["config"]["LiveDataFunctionCodePos"]);
  
  Conf_RequestLiveData->clear();
  for (JsonArray arr : doc[this->InverterType.name]["config"]["RequestLiveData"].as<JsonArray>()) {
  
    std::vector<byte> t = {};
    for (String x : arr) {
      byte e = this->String2Byte(x);
      t.push_back(e);
    }
    t.push_back(DATAISLIVE); // last byte is datatype
    Conf_RequestLiveData->push_back(t);
  }
  
  Conf_RequestIdData->clear();
  for (JsonArray arr : doc[this->InverterType.name]["config"]["RequestIdData"].as<JsonArray>()) {

    std::vector<byte> t = {};
    for (String x : arr) {
      byte e = this->String2Byte(x);
      t.push_back(e);
    }
    t.push_back(DATAISID); // last byte is datatype
    Conf_RequestIdData->push_back(t);
  }

  if (regfile) { regfile.close(); }

}

/*******************************************************
 * convert a particular string from JSON to a byte
*******************************************************/
byte modbus::String2Byte(String s){
  byte ret = 0x00;
  if(s.startsWith("0x")) {
    ret = strtoul(s.substring(2).c_str(), NULL, 16);
  } else if(s.equalsIgnoreCase("#clientid")) {
    ret = this->ClientID;
  }
  return ret;
}

/*******************************************************
 * Enable MQTT Transmission
*******************************************************/
void modbus::enableMqtt(MQTT* object) {
  this->mqtt = object;

  // subscribe to all active setters
  for (uint8_t i = 0; i < this->Setters->size(); i++) {
    if (this->Setters->at(i).active) {
      this->mqtt->Subscribe(this->GetMqttSetTopic(this->Setters->at(i).Name));
    }
  }
}

/*******************************************************
 * Query ID Data to Inverter
*******************************************************/
void modbus::QueryIdData() {
  Config->logN(4, "Query ID Data into Queue:");
  
  /* byte message[] = {this->ClientID, 
                               0x03,  // FunctionCode
                               0x00,  // StartAddress MSB
                               0x00,  // StartAddress LSB
                               0x00,  // Anzahl Register MSB
                               0x14,  // Anzahl Register LSB
                               0x00,  // CRC LSB
                               0x00   // CRC MSB
           }; // 
  */

  if (this->ReadQueue->isEmpty()) {
    for (uint8_t i = 0; i < this->Conf_RequestIdData->size(); i++) {
      Config->logN(4, this->PrintDataFrame(&this->Conf_RequestIdData->at(i)).c_str());
      this->ReadQueue->enqueue(this->Conf_RequestIdData->at(i));
    }
    this->LastTxIdData = millis(); //erst setzen wenn erfolgreich in die Queue geschickt wurde
  }
}

/*******************************************************
 * Query Live Data to Inverter
*******************************************************/
void modbus::QueryLiveData() {
  Config->logN(4, "Query Live Data into Queue:");
   
  /* byte message[] = {this->ClientID, 
                               0x04,  // FunctionCode
                               0x00,  // StartAddress MSB
                               0x00,  // StartAddress LSB
                               0x00,  // Anzahl Register MSB
                               0x23,  // Anzahl Register LSB
                               0x00,  // CRC LSB
                               0x00   // CRC MSB
           }; // 
  */

  if (this->ReadQueue->isEmpty()) {
    for (uint8_t i = 0; i < this->Conf_RequestLiveData->size(); i++) {
      Config->logN(4, this->PrintDataFrame(&this->Conf_RequestLiveData->at(i)).c_str());
      this->ReadQueue->enqueue(this->Conf_RequestLiveData->at(i));
    }
    this->LastTxLiveData = millis(); //erst setzen wenn erfolgreich in die Queue geschickt wurde
  }
}

/*******************************************************
 * send 1 Read query from queue to inverter 
*******************************************************/
void modbus::QueryQueueToInverter() {
  enum rwtype_t {READ, WRITE, NUL};
  rwtype_t rwtype;
  std::vector<byte> m = {};
  byte rememberdatatype = 0;

  if (!this->SetQueue->isEmpty()) {
    rwtype = WRITE;
    m = this->SetQueue->dequeue();
  } // writing has priority
  else if (!this->ReadQueue->isEmpty()) {
    rwtype = READ;
    m = this->ReadQueue->dequeue();
    rememberdatatype = m[m.size()-1];  // store current datatype
    m.pop_back();                      // shorten queue item to original size
  }
  else { rwtype = NUL; }

  if (rwtype != NUL) {
    Config->logN(3, "Request queue data to inverter: ");
  
    digitalWrite(this->pin_RTS, RS485Receive);     // init Receive
    while (RS485Serial->available() > 0) { // read serial if any old data is available
      RS485Serial->read();
    }

    byte message[m.size() + 2] = {0x00}; // +2 Byte CRC
  
    for (uint8_t i=0; i < m.size(); i++) {
      message[i] = m.at(i);
    }

    uint16_t crc = this->Calc_CRC(message, sizeof(message)-2);
    message[sizeof(message)-1] = highByte(crc);
    message[sizeof(message)-2] = lowByte(crc);
    m.push_back(lowByte(crc));
    m.push_back(highByte(crc));

    Config->logN(3, this->PrintDataFrame(message, sizeof(message)).c_str());

    digitalWrite(this->pin_RTS, RS485Transmit);     // init Transmit
    RS485Serial->write(message, sizeof(message));
    RS485Serial->flush();
    digitalWrite(this->pin_RTS, RS485Receive); 

    // wait 100ms to get response
    unsigned long timeout=millis()+100;
    while (millis()<=timeout) { yield(); }

    if (rwtype == WRITE) {
      this->ReceiveSetData(&m);
      this->LastTxIdData = 0; //Setze den Timer zurück um nach einem Set Befehl die ID Daten abzufragen zwecks Rückmeldung ob der Set Befehl ausgeführt wurde
    } 
    else if (rwtype == READ) { 
      this->ReceiveReadData();
      if (this->ReadQueue->isEmpty()) {
        this->DataFrame->push_back(rememberdatatype);  // add datatype at the end of the received frame
        this->ParseData();
      }
    }

  }
}

/***********************************************************
 * Receive Data after a Set Query, Check if successful set
************************************************************/
bool modbus::ReceiveSetData(std::vector<byte>* SendHexFrame) {
  std::vector<byte> RecvHexframe = {};
  bool ret = false; 

  Config->logN(3, "Read Data from Queue: ");

  digitalWrite(this->pin_RTS, RS485Receive);     // init Receive
  if (RS485Serial->available()) {
    // lese alle Daten, speichern im Vektor "Dataframe"
    while(RS485Serial->available()) {
      byte d = RS485Serial->read();
      RecvHexframe.push_back(d);
      Config->log(4, "%s ", PrintHex(d));
      delay(1); // keep this! Loosing bytes possible if too fast
    }    
    Config->logN(4, "");
    
    // TODO
    // compare Set and Received Answer, should be equal
    //if (SendHexFrame == RecvHexframe) {
      // successful
      //ret = true;
    //}
  }
  return ret;
}

/*******************************************************
 * Receive Data after Quering, put them into vector
*******************************************************/
void modbus::ReceiveReadData() {
  size_t dataFrameStartPos =  this->DataFrame->size();

  Config->logN(3, "Read Data from Queue: ");

  digitalWrite(this->pin_RTS, RS485Receive);     // init Receive
  if (RS485Serial->available()) {
    // lese alle Daten, speichern im Vektor "Dataframe"
    while(RS485Serial->available()) {
      byte d = RS485Serial->read();
      this->DataFrame->push_back(d);
      Config->log(4, "%s", PrintHex(d).c_str());
      delay(1); // keep this! Loosing bytes possible if too fast
    }
    Config->logN(4, "");
    
    bool valid = true;

    if (this->DataFrame->size()-dataFrameStartPos > 5 &&
        this->DataFrame->at(dataFrameStartPos+this->Conf_ClientIdPos) == this->ClientID && 
        this->DataFrame->at(dataFrameStartPos+this->Conf_IdDataErrorPos) != this->Conf_IdDataErrorCode && 
        this->DataFrame->at(dataFrameStartPos+this->Conf_LiveDataErrorPos) != this->Conf_LiveDataErrorCode) {

      Config->logN(4, "ErrorCode passed, OK");
      
      if (this->enableCrcCheck) {
        //CRC Check
        uint16_t crc = this->Calc_CRC(this->DataFrame, dataFrameStartPos, this->DataFrame->size()-2);
        
        Config->logN(4, "Received CRC: 0x%02X 0x%02X", this->DataFrame->at(this->DataFrame->size()-2), this->DataFrame->at(this->DataFrame->size()-1));
        Config->logN(4, "Calculated CRC: 0x%02X 0x%02X", lowByte(crc), highByte(crc));

        if (this->DataFrame->at(this->DataFrame->size()-2) != lowByte(crc) ||
            this->DataFrame->at(this->DataFrame->size()-1) != highByte(crc)) {
          valid = false;
          Config->logN(2, "CRC check  failed!");
        }
      }

      if (this->enableLengthCheck) {
        // Check datalength
        Config->logN(4, "Dataframe length should be: %d, is: %d bytes", this->DataFrame->at(dataFrameStartPos+2), this->DataFrame->size()-dataFrameStartPos-5);

        if (this->DataFrame->at(dataFrameStartPos+2) != this->DataFrame->size()-dataFrameStartPos-5) {
          valid = false;
          Config->logN(2, "data length check failed, should be %d but is %d bytes", this->DataFrame->at(dataFrameStartPos+2), this->DataFrame->size()-dataFrameStartPos-5);
        }
      } 
    } else { valid = false; } 
    
    if (valid) {
      // Dataframe valid
      Config->logN(3, "Dataframe valid, Dateframe size: %d bytes", this->DataFrame->size());

    } else {
      Config->logN(2, "Dataframe invalid");
      // clear dataframe, clear ReadQueue to start from fresh
      this->DataFrame->clear();
      for (unsigned int n = 0; n < this->ReadQueue->itemCount(); n++) {
        this->ReadQueue->dequeue();
      }
    }

  } else {
    Config->logN(2, "no response from client");
  }
}

/*******************************************************
 * Helper function
 * convert a json position array to integer values of Dataframe
*******************************************************/
int modbus::JsonPosArrayToInt(JsonArray posArray, JsonArray posArray2) {
  uint32_t val_i = 0;
  uint32_t val2_i = 0;
  uint32_t val_max = 0;

  // handle Main Position Array
  if (!posArray.isNull()){ 
    for(uint16_t v : posArray) {
      if (v < this->DataFrame->size()) { 
        val_i = (val_i << 8) | this->DataFrame->at(v); 
        val_max = (val_max << 8) | 0xFF;
      } else Config->logN(1, "Error: position %d out of dataframe range", v);
    }
  }

  // handle Position Array 2
  if (!posArray2.isNull()){ 
    for(uint16_t w : posArray2) {
      if (w < this->DataFrame->size()) { 
        val2_i = (val2_i << 8) | this->DataFrame->at(w); 
      } else Config->logN(1, "Error: position %d out of dataframe range", w);
    }
  }

  // Check Type 1: convert into a negative value

  if (posArray2.isNull() && (val_i > (val_max/2))) {
     // negative Zahl, umrechnen notwendig 
    val_i = val_i ^ val_max;
    val_i = val_i * -1;
  } 

  // Check type 2: convert into a negative value
  // an extra position field for a neg value existis. Take this if value > 0
  if (val2_i > 0) {
    val_i = val2_i * -1;
  } 

  return val_i;
}

/*******************************************************
 * Parse all received Data in vector 
*******************************************************/
void modbus::ParseData() {
  char buffer[100] = {0}; 
  memset(buffer, 0, sizeof(buffer));
  
  String RequestType = "";
  byte tempbyte;

  if (this->DataFrame->size() <= 1) {

    // TODO
    //  ***********************************************
    // do some tests if client isn´t connected, dataframe is empty
    //  ***********************************************
    #ifdef DEBUGMODE 
      this->DataFrame->clear();
      Config->logN(3, "Start parsing in testmode, use some testdata instead real live data :)");
      
      // Solar-KTL 
      //byte ReadBuffer[] = {0x01, 0x03, 0x60, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x34, 0x01, 0xF3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x13, 0x86, 0x09, 0x11, 0x01, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x4C, 0x00, 0x00, 0x02, 0x5F, 0x00, 0x8A, 0x01, 0x7D, 0x00, 0x28, 0x00, 0x33, 0x0E, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x60, 0x01, 0x42, 0x0A, 0x3B, 0x00, 0x0E, 0x00, 0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0x25,0x01};

      // Solax MIC
      //byte ReadBuffer[] = {0x01, 0x04, 0x80, 0x12, 0x34, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x09, 0x64, 0x09, 0x67, 0x09, 0x6B, 0x13, 0x8C, 0x13, 0x8D, 0x13, 0x8B, 0x00, 0x27, 0x00, 0x28, 0x00, 0x28, 0x00, 0x2C, 0x0B, 0x54, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x70, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0xC3,0x02s};
      
      //Solax X1
      byte ReadBuffer[] = {0x01,0x04,0xEE,0x09,0x29,0x00,0x5E,0x08,0x9E,0x0B,0xFA,0x0B,0x44,0x00,0x16,0x00,0x39,0x13,0x8A,0x00,0x26,0x00,0x02,0x02,0xB8,0x06,0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0xB6,0x00,0x00,0x03,0xAC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0A,0x00,0x00,0x6E,0xFB,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0E,0x33,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xB9,0xA4,0x01,0x04,0xEE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x59,0x77,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x69,0x00,0x00,0x86,0x6B,0x00,0x01,0x00,0x0B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x70,0x00,0x00,0x03,0xF2,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0xB9,0x02};
      //byte ReadBuffer[] = {0x01,0x04,0xEE,0x09,0x29,0x00,0x5E,0x08,0x9E,0x0B,0xFA,0x0B,0x44,0x00,0x16,0x00,0x39,0x13,0x8A,0x00,0x26,0x00,0x02,0x02,0xB8,0x06,0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0xB6,0x00,0x00,0x03,0xAC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0A,0x00,0x00,0x6E,0xFB,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0E,0x33,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xB9,0xA4,0x02};
      //byte ReadBuffer[] = {0x01,0x03,0x28,0x48,0x34,0x35,0x30,0x32,0x41,0x49,0x34,0x34,0x35,0x39,0x30,0x30,0x35,0x73,0x6F,0x6C,0x61,0x78,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x4A,0xA0, 0x01};
      
      //Growatt IDData
      //byte ReadBuffer[] = {0x01,0x03,0xEE,0x00,0x01,0x00,0xBD,0xFF,0xFF,0x00,0x64,0x00,0x00,0x27,0x10,0x00,0x00,0x9C,0x40,0x06,0x40,0x44,0x4E,0x31,0x2E,0x30,0x00,0x5A,0x42,0x44,0x42,0x00,0x02,0x00,0x02,0x00,0x00,0x06,0x40,0x00,0x3C,0x00,0x3C,0x00,0x5A,0x00,0x5A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x64,0x00,0x00,0x00,0x00,0x20,0x20,0x20,0x50,0x56,0x20,0x49,0x6E,0x76,0x65,0x72,0x74,0x65,0x72,0x20,0x20,0x00,0x00,0x15,0x18,0x02,0x03,0x07,0xE8,0x00,0x07,0x00,0x13,0x00,0x08,0x00,0x10,0x00,0x05,0x00,0x05,0x0C,0x73,0x13,0x73,0x12,0x8E,0x14,0x1E,0x07,0x00,0x13,0x73,0x12,0x8E,0x14,0x1E,0x07,0x00,0x13,0x73,0x12,0x8E,0x14,0x1E,0x0D,0x61,0x10,0xF6,0x12,0x9D,0x13,0x8D,0x00,0x98,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x00,0x32,0x11,0x1E,0x00,0x00,0x44,0x4E,0x41,0x41,0x30,0x31,0x35,0x31,0x30,0x30,0x30,0x32,0x01,0x31,0x00,0x00,0x00,0x00,0x13,0x9C,0x00,0x32,0x10,0x07,0x10,0xA6,0x0F,0x18,0x0E,0x79,0x00,0x14,0x00,0x05,0x10,0x57,0x0F,0x90,0x26,0xFD,0x26,0xFD,0x26,0xA5,0x27,0xC1,0x27,0xC1,0x28,0x21,0x00,0x0A,0x00,0x00,0x01,0xE4,0x00,0xFF,0x4E,0x20,0x00,0xFF,0x4E,0x20,0x00,0xFF,0x4E,0x20,0x00,0xFF,0x4E,0x20,0x07,0x00,0xFC,0xE5,0x01};
      //byte ReadBuffer[] = {0x01,0x03,0x40,0x00,0x01,0x08,0x40,0x00,0x00,0x00,0x64,0x00,0x64,0x27,0x10,0x00,0x00,0x0B,0xB8,0x0E,0x10,0x52,0x41,0x31,0x2E,0x30,0x20,0x5A,0x43,0x42,0x43,0x00,0x05,0x00,0x02,0x00,0x00,0x03,0x52,0x00,0x1E,0x00,0x3C,0x00,0xC8,0x00,0x64,0x00,0x00,0x44,0x54,0x4D,0x34,0x45,0x35,0x4A,0x30,0x30,0x4E,0x01,0x00,0xF2,0x27,0x00,0x01,0x00,0x00,0xCC,0xF4,0x01};

      //Growatt SPH LiveData
      //byte ReadBuffer[] = {0x01,0x04,0xEE,0x00,0x01,0x00,0x00,0x32,0x8F,0x07,0xF5,0x00,0x00,0x00,0x00,0x00,0x00,0x0B,0xC2,0x00,0x2B,0x00,0x00,0x32,0x8F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0xED,0x13,0x86,0x09,0x5B,0x00,0x06,0x00,0x00,0x05,0x9D,0x09,0x5C,0x00,0x06,0x00,0x00,0x05,0x9D,0x09,0x4D,0x00,0x06,0x00,0x00,0x05,0x94,0x10,0x54,0x10,0x1B,0x10,0x1B,0x00,0x00,0x00,0x03,0x00,0x00,0x5F,0x9F,0x01,0xA8,0xF8,0x7A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0B,0x00,0x00,0x67,0x99,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x67,0x99,0x01,0xBD,0x01,0x63,0x01,0x54,0x00,0x00,0x01,0x79,0x0C,0xD4,0x0C,0xCA,0x4E,0x20,0x00,0x00,0x00,0x00,0x9C,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0xFA,0x01,0x04,0xEE,0x4E,0x20,0x00,0x03,0x00,0x00,0x9C,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x0A,0x25,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x02,0x00,0x00,0x20,0x19,0x00,0x00,0x00,0x08,0x00,0x00,0x21,0xD5,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x51,0x00,0x00,0x66,0xD9,0x00,0x00,0x00,0x05,0x00,0x00,0x50,0x16,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x27,0x10,0x00,0x00,0x19,0xA0,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x53,0x00,0x00,0x30,0x00,0x12,0x19,0x9E,0x0D,0x4A,0x00,0x15,0x00,0x15,0x01,0x61,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x28,0x14,0x00,0x00,0x20,0x19,0x00,0x00,0x21,0xD5,0x00,0x00,0x00,0x03,0x0C,0x54,0x00,0x39,0x00,0x10,0x00,0x00,0x01,0x01,0x00,0xE1,0x00,0x02,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x31,0x00,0x31,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x12,0x52,0x94,0x01,0xCC,0x01,0xA5,0xD1,0x6F,0xEE,0x00,0x01,0x00,0x00,0x32,0x8F,0x07,0xF5,0x00,0x00,0x00,0x00,0x00,0x00,0x0B,0xC2,0x00,0x2B,0x00,0x00,0x32,0x8F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0xED,0x13,0x86,0x09,0x5B,0x00,0x06,0x00,0x00,0x05,0x9D,0x09,0x5C,0x00,0x06,0x00,0x00,0x05,0x9D,0x09,0x4D,0x00,0x06,0x00,0x00,0x05,0x94,0x10,0x54,0x10,0x1B,0x10,0x1B,0x00,0x00,0x00,0x03,0x00,0x00,0x5F,0x9F,0x01,0xA8,0xF8,0x7A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0B,0x00,0x00,0x67,0x99,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x67,0x99,0x01,0xBD,0x01,0x63,0x01,0x54,0x00,0x00,0x01,0x79,0x0C,0xD4,0x0C,0xCA,0x4E,0x20,0x00,0x00,0x00,0x00,0x9C,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0xFA,0x01,0x04,0xEE,0x4E,0x20,0x00,0x03,0x00,0x00,0x9C,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x0A,0x25,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x02,0x00,0x00,0x20,0x19,0x00,0x00,0x00,0x08,0x00,0x00,0x21,0xD5,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x51,0x00,0x00,0x66,0xD9,0x00,0x00,0x00,0x05,0x00,0x00,0x50,0x16,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x27,0x10,0x00,0x00,0x19,0xA0,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x53,0x00,0x00,0x30,0x00,0x12,0x19,0x9E,0x0D,0x4A,0x00,0x15,0x00,0x15,0x01,0x61,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x28,0x14,0x00,0x00,0x20,0x19,0x00,0x00,0x21,0xD5,0x00,0x00,0x00,0x03,0x0C,0x54,0x00,0x39,0x00,0x10,0x00,0x00,0x01,0x01,0x00,0xE1,0x00,0x02,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x31,0x00,0x31,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x12,0x52,0x94,0x01,0xCC,0x01,0xA5,0xD1,0x6F,0x02};
      
      // K8H
      //byte ReadBuffer[] = {0xF7,0x02,0x1E,0x0B,0x3E,0x00,0x39,0x05,0xE1,0x0A,0x68,0x00,0x39,0x05,0xFA,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0xFC,0x00,0x7C,0x00,0x00,0x13,0x87,0x00,0x00,0x24,0x27, 0x02};

      // Solax-X3
      //byte ReadBuffer[] = {0x01,0x04,0x42,0x17,0xB4,0x12,0x21,0x00,0x02,0x00,0x02,0x13,0x88,0x00,0x27,0x00,0x02,0x00,0x7A,0x00,0x7A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xAA,0xDB,0x01,0x04,0x2C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x48,0xFF,0xFF,0x05,0x2F,0x00,0x02,0x7E,0xDD,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1B,0x00,0x00,0x4D,0x6E,0x00,0x00,0x00,0x01,0x00,0x00,0xD3,0x0A,0x01,0x04,0x64,0x08,0x4A,0x00,0x09,0x00,0x64,0x13,0x88,0x08,0x81,0x00,0x09,0x00,0x3D,0x13,0x88,0x08,0x20,0x00,0x09,0x00,0x61,0x13,0x89,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x95,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0x2D,0xFF,0xFF,0x31,0xA8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x4A,0xC0,0x00,0x00,0x00,0x1B,0x00,0x00,0x00,0x47,0x00,0x00,0x01,0xD9,0x00,0x00,0x55,0x5B,0x01,0x04,0x04,0x00,0x00,0x00,0x00,0xFB,0x84,0x02};

      for (uint16_t i = 0; i<sizeof(ReadBuffer); i++) {
        this->DataFrame->push_back(ReadBuffer[i]);
      }
      Config->logN(4, "%s", this->PrintDataFrame(this->DataFrame).c_str());
    #endif
    // ***********************************************
  } 
  
  if (this->DataFrame->size() > 1) {
    tempbyte = this->DataFrame->at(this->DataFrame->size()-1);
    this->DataFrame->pop_back();   // shorten to original size
    
    // setup RequestType
    if (tempbyte == DATAISID) {
      RequestType = "id";
    }
    else if (tempbyte == DATAISLIVE) {
      RequestType = "livedata";
    }

    Config->logN(3, "parse %d bytes of data", this->DataFrame->size());
    Config->logN(4, "identified datatype: %s", RequestType.c_str());
    
    File regfile = LittleFS.open("/regs/"+this->InverterType.filename);
    if (!regfile) {
      Config->logN(1, "failed to open %s file", this->InverterType.filename.c_str());
    }
    String streamString = "";
    streamString = "\""+ this->InverterType.name +"\": {";
    regfile.find(streamString.c_str());
    
    streamString = "\""+ RequestType +"\": [";
    regfile.find(streamString.c_str());
    do {
      JsonDocument elem;
      DeserializationError error = deserializeJson(elem, regfile); 
      
      if (!error) {
        // Print the result
        Config->logN(4, "parsing JSON ok");
        Config->log(5, elem);
      } else {
        Config->logN(1, "(Function ParseData) Failed to parse JSON Register Data: %s", error.c_str()); 
      }

      // setUp local variables
      String datatype = "";
      String openwbtopic = "";
      float factor = 1;
      int valueAdd = 0;
      String unit = "";

      JsonArray posArray, posArray2;
      float val_f = 0;
      int val_i = 0;
      String val_str = "";
      reg_t d = {};
      bool IsActiveItem = false;
      
      
      // mandantory field
      if(!elem["name"].isNull()) {
        d.Name = elem["name"].as<String>();
      } else {
        d.Name = String("undefined");
      }
      
      // mandantory field
      // check if "position" is a well defined array
      if (elem["position"].is<JsonArray>()) {
        posArray = elem["position"].as<JsonArray>();
      } else {
        Config->logN(1, "Error: for Name '%s' no position array found", d.Name.c_str());
        continue;
      }

      // mandantory field
      if(!elem["datatype"].isNull()) {
        datatype = elem["datatype"].as<String>();
        datatype.toLowerCase();
      } 
      
      // optional field
      if(this->Conf_EnableOpenWB && !elem["openwbtopic"].isNull()) {
        openwbtopic = elem["openwbtopic"].as<String>();
      }
      
      // optional field
      if (elem["factor"]) {
        factor = elem["factor"];
      }

      // optional field
      if (elem["valueAdd"]) {
        valueAdd = elem["valueAdd"];
      }
      
      // optional field
      if (elem["unit"]) {
        unit = elem["unit"].as<String>();
      }
      d.unit = unit;

      // optional field 
      // check, if a extra position for a negative value was defined
      if (elem["position2"].is<JsonArray>()) {
        posArray2 = elem["position2"].as<JsonArray>();
      } 

      // check if LiveData item is active to send data out via mqtt
      for (uint16_t i=0; i < this->InverterLiveData->size(); i++) {
        if (this->InverterLiveData->at(i).Name == d.Name && this->InverterLiveData->at(i).active) {
          IsActiveItem = true;
        }
      }
      
      // check if ID-Data item is active to send data out via mqtt
      for (uint16_t i=0; i < this->InverterIdData->size(); i++) {
        if (this->InverterIdData->at(i).Name == d.Name && this->InverterIdData->at(i).active) {
         IsActiveItem = true;
        }
      }
      
      // ************* processing data ******************
      if (datatype == "float") {
        //********** handle Datatype FLOAT ***********//
        val_f = (float)(this->JsonPosArrayToInt(posArray, posArray2) * factor) + valueAdd;
        sprintf(buffer, "%.2f", val_f);
        d.value = String(buffer);
      
      } else if (datatype == "integer") {
        //********** handle Datatype Integer ***********//
        val_i = (this->JsonPosArrayToInt(posArray, posArray2) * factor) + valueAdd;
        sprintf(buffer, "%d", val_i);
        d.value = String(buffer);

      } else if (datatype == "binary") {
        //********** handle Datatype Integer ***********//
        val_i = (this->JsonPosArrayToInt(posArray, posArray2) * factor) + valueAdd;
        d.value = this->ConvertIntToBinaryString(val_i, posArray.size() * 8);
      
      } else if (datatype == "string") {
        //********** handle Datatype String ***********//
        if (!posArray.isNull()) { 
          char buffer[posArray.size()+1];
          uint8_t i=0;
          for(int v : posArray) {
            if (v < DataFrame->size()) {
              buffer[i] = static_cast<char>(DataFrame->at(v));
              i++;
            } else Config->logN(1, "Error: for item '%s' position %d out of dataframe range", d.Name.c_str(), v);
          }
          buffer[i] = '\0';
          d.value = String(buffer);
        } 
      } else {
        //****************** sonst, leer *******************//
        d.value = "";
        Config->logN(2, "Error: for Name '%s' no valid datatype found", d.Name.c_str());
      }


      // map values if a mapping is specified
      if(!elem["mapping"].isNull() && elem["mapping"].is<JsonArray>() && d.value != "") {
        Config->logN(4, "Map values for item %s", d.Name.c_str());

        JsonArray map = elem["mapping"].as<JsonArray>();
        if (datatype == "binary") d.value = this->MapBitwise(map, d.value);
        else d.value = this->MapItem(map, d.value);
      }

      Config->logN(4, "Data: %s -> %s %s", d.Name.c_str(), d.value.c_str(), d.unit.c_str());

      if (this->mqtt && IsActiveItem && d.Name != "") { 
          this->mqtt->Publish_String(d.Name.c_str(), d.value, false);

          if (openwbtopic.length() > 0) { 
            const String newTopic(OpenWB->getOpenWbTopic(openwbtopic));
            if (newTopic.length() > 0) {
              this->mqtt->Publish_String(newTopic.c_str(), d.value, true);
            }
          }
      }

      if(RequestType == "livedata") {
        this->ChangeRegItem(this->InverterLiveData, d);
      } else if(RequestType == "id") {
        this->ChangeRegItem(this->InverterIdData, d);
        
        Config->logN(3, "Inverter ID Data found -> %s: %s ", d.Name.c_str(), d.value.c_str());

      }

      //if (webSocketCallback) {
        //const String ws("{\"data-id\":{\"" + d.Name + ".value\":\"" + d.value + " "+ d.unit +"\"}}");
        //const String ws("{\"" + d.Name + ".value\":\"" + d.value + " "+ d.unit +"\"}");
        //webSocketCallback(ws);
      //}
    
    } while (regfile.findUntil(",","]"));

    if (regfile) { regfile.close(); }

  } 

  // all data were process, clear Dataframe now for next query
  // save this for WebGUI "RAW-Data" first
  if(RequestType == "livedata") {
    this->SaveLiveDataframe->clear();
    this->SaveLiveDataframe->assign(this->DataFrame->begin(), this->DataFrame->end());
  } else if(RequestType == "id") {
    this->SaveIdDataframe->clear();
    this->SaveIdDataframe->assign(this->DataFrame->begin(), this->DataFrame->end());
  }

  if (webSocketCallback) {
    this->SendDataToWebSocket(RequestType == "livedata" ? this->InverterLiveData : this->InverterIdData);
  }

  this->DataFrame->clear();   
}

void modbus::SendDataToWebSocket(std::vector<reg_t>* vector) {
  if (webSocketCallback) {
    String msg("{\"data-id\":{"); msg.reserve(vector->size() * 25);

    for (uint8_t i=0; i < vector->size(); i++) {
      if (i > 0) msg += ",";
      msg += "\"" + vector->at(i).Name + ".value\":\"" + vector->at(i).value + " "+ vector->at(i).unit +"\"";
    }
    msg += "}}";
    webSocketCallback(msg);
  }
}

String modbus::ConvertIntToBinaryString(int n, int numBits) {
  String binaryString = "";
  binaryString.reserve(numBits);

  for (int i = numBits - 1; i >= 0; i--) {
    binaryString += ((n >> i) & 1) ? "1" : "0";
  }
  return binaryString;
}

/*******************************************************
 * Map a Binary to a predefined constant string
*******************************************************/
String modbus::MapBitwise(JsonArray map, String value) {
  String ret("");
  
  for (uint8_t i=0; i<value.length(); i++) {
    if (String(value[i]) == "1") {
      //note: 1 item less than map size, because last item is default value
      Config->logN(4, "Mapped value: %s -> %s\n", String(value[i]).c_str(), map[map.size() -2 -i].as<String>().c_str());
      if (ret.length() > 0) ret += ", ";
      if ((map.size() -2 -i) >=0 && map[map.size() -2 -i]) ret += map[map.size() -2 -i].as<String>();
      else ret += "undefined";
    }
  }

  // if nothing found, set default value (is last item in array)
  if (ret.length() == 0) ret = map[map.size() - 1].as<String>();
  return ret;              
}

/*******************************************************
 * Map a value to a predefined constant string
*******************************************************/
String modbus::MapItem(JsonArray map, String value) {
  String ret = value;

  for (JsonArray mapItem : map) {
    String v1 = mapItem[0].as<String>();
    String v2 = mapItem[1].as<String>();

    v1.toLowerCase();
    v2.toLowerCase();
    value.toLowerCase();

    if (value == v1) {
      ret = v2;
      Config->logN(4, "Mapped value: %s -> %s", v1.c_str(), v2.c_str());
    }
  } 
  return ret;
}

/*******************************************************
 * Change a Register Item with current data
*******************************************************/
void modbus::ChangeRegItem(std::vector<reg_t>* vector, reg_t item) {
  for (uint16_t i=0; i<vector->size(); i++) {
    if (vector->at(i).Name == item.Name) {
      vector->at(i).value = item.value;
      vector->at(i).unit = item.unit;
      break;
    }
  }
}

/*******************************************************
 * Calculate CRC Checksum
*******************************************************/
uint16_t modbus::Calc_CRC(uint8_t* message, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)message[pos];          // XOR byte into least sig. byte of crc
    for (int i = 8; i != 0; i--) {          // Loop over each bit
      if ((crc & 0x0001) != 0) {            // If the LSB is set
        crc >>= 1;                          // Shift right and XOR 0xA001
        crc ^= 0xA001;
      }
      else                                  // Else LSB is not set
        crc >>= 1;                          // Just shift right
    }
  }
  return crc;
}  

uint16_t modbus::Calc_CRC(std::vector<byte>* message, uint16_t startpos, uint16_t endpos) {
  uint16_t crc = 0xFFFF;
  for (uint16_t pos = startpos; pos < endpos; pos++) {
    crc ^= (uint16_t)message->at(pos);      // XOR byte into least sig. byte of crc
    for (int i = 8; i != 0; i--) {          // Loop over each bit
      if ((crc & 0x0001) != 0) {            // If the LSB is set
        crc >>= 1;                          // Shift right and XOR 0xA001
        crc ^= 0xA001;
      }
      else                                  // Else LSB is not set
        crc >>= 1;                          // Just shift right
    }
  }
  return crc;
}

/*******************************************************
 * friendly output of hex nums
*******************************************************/
String modbus::PrintHex(byte num) {
  char hexCar[5] = {0};
  memset(hexCar, 0, sizeof(hexCar));
  snprintf(hexCar, sizeof(hexCar), "0x%02X", num);
  return String(hexCar);
}

/*******************************************************
 * friendly output the entire received Dataframe
*******************************************************/
String modbus::PrintDataFrame(std::vector<byte>* frame) {
  String out = "";
  for (uint16_t i = 0; i<frame->size(); i++) {
    if (out.length() < 2040) { // max string length 2048
      out.concat(this->PrintHex(frame->at(i)));
      out.concat(" ");
    }
  }
  return out;
}

String modbus::PrintDataFrame(byte* frame, uint8_t len) {
  String out = "";
  for (uint16_t i = 0; i<len; i++) {
    out.concat(this->PrintHex(frame[i]));
    out.concat(" ");
  }
  return out;
}

/*******************************************************
 * make InverterSN callable from outside
*******************************************************/
String modbus::GetInverterSN() {
  String sn = "unknown";
  
  for (uint16_t i=0; i < this->InverterIdData->size(); i++) {
    if (this->InverterIdData->at(i).Name == "InverterSN")
      return this->InverterIdData->at(i).value;
  }
  return sn;
}

/*******************************************************
 * Return all LiveData as jsonArray
 * {data: [{"name": "xx", "value": "xx", ...}, ...] }
*******************************************************/
void modbus::GetLiveDataAsJsonToWebServer(AsyncWebServerRequest *request) {
  std::shared_ptr<uint16_t> counter = std::make_shared<uint16_t>(0);
  std::shared_ptr<bool> firstRow = std::make_shared<bool>(true);

  String subaction(""), json("{}");
  
  if(request->hasArg("json")) {
    json = request->arg("json");
  }
  JsonDocument jsonGet; 
  DeserializationError error = deserializeJson(jsonGet, json.c_str());
  
  Config->logN(4, "[GetLiveDataAsJsonToWebServer] Json command empfangen: ");
  if (!error) {
    Config->log(4, jsonGet);

    if (jsonGet["cmd"]["subaction"]) subaction = jsonGet["cmd"]["subaction"].as<String>();
  
  } else { 
    Config->logN(2, "[GetLiveDataAsJsonToWebServer] Json Command not parseable: %s -> %s", json.c_str(), error.c_str());
  }

	AsyncWebServerResponse *response = request->beginChunkedResponse("application/json", [this, firstRow, counter, subaction](uint8_t *buffer, size_t maxLen, size_t index) {
			String ret("");
      ret.reserve(maxLen);
      maxLen -= 500; // use a puffer of 500 bytes, every item is assumed to be 200 bytes

      if (*counter == 0) {
        // send start of JSON
        ret += "{\"data\": {\"items\": [";
        (*counter)++;
      }
      

      if (*counter <= this->InverterIdData->size() && ret.length() < maxLen) {
        // send IdData
        uint16_t i = *counter - 1;

        // jedes JsonObject wird mit 200 bytes angenommen, + 100 bytes puffer am Ende
        while (i < this->InverterIdData->size() && ret.length() < maxLen) {
          if (!(subaction == "onlyactive" && !this->InverterIdData->at(i).active)) {
            if(!(*firstRow)) ret += ",";
            ret += "{\"name\": \"" + this->InverterIdData->at(i).Name + "\",";
            ret += "\"realname\": \"" + this->InverterIdData->at(i).RealName + "\",";
            ret += "\"value\": {\"innerHTML\": \"" + this->InverterIdData->at(i).value + " " + this->InverterIdData->at(i).unit + "\", \"data-id\": \"" + this->InverterIdData->at(i).Name + ".value" + "\"},";
            ret += "\"active\": {\"checked\": " + String(this->InverterIdData->at(i).active ? 1 : 0) + ", \"name\": \"" + this->InverterIdData->at(i).Name + "\"},";
            ret += "\"mqtttopic\": \"" + this->mqtt->getTopic(this->InverterIdData->at(i).Name, false) + "\"";
            
            if (this->Conf_EnableOpenWB && this->InverterIdData->at(i).openwb.length() > 0) {
              ret += ",\"openwb\": [{\"openwbtopic\": \"" + OpenWB->getOpenWbTopic(this->InverterIdData->at(i).openwb) + "\"}]";
            }
            ret += "}";
            (*firstRow) = false;
          }

          (*counter)++;
          i++;
        }
      } 
      
      if (*counter <= this->InverterIdData->size() + this->InverterLiveData->size() && ret.length() < maxLen) {
        // send LiveData
        uint16_t i = *counter - this->InverterIdData->size() - 1;
        
        while (i < this->InverterLiveData->size() && ret.length() < maxLen) {
          if (!(subaction == "onlyactive" && !this->InverterLiveData->at(i).active)) {
            if(!(*firstRow)) ret += ",";
            ret += "{\"name\": \"" + this->InverterLiveData->at(i).Name + "\",";
            ret += "\"realname\": \"" + this->InverterLiveData->at(i).RealName + "\",";
            ret += "\"value\": {\"innerHTML\": \"" + this->InverterLiveData->at(i).value + " " + this->InverterLiveData->at(i).unit + "\", \"data-id\": \"" + this->InverterLiveData->at(i).Name + ".value" + "\"},";
            ret += "\"active\": {\"checked\": " + String(this->InverterLiveData->at(i).active ? 1 : 0) + ", \"name\": \"" + this->InverterLiveData->at(i).Name + "\"},";
            ret += "\"mqtttopic\": \"" + this->mqtt->getTopic(this->InverterLiveData->at(i).Name, false) + "\"";
            
            if (this->Conf_EnableOpenWB && this->InverterLiveData->at(i).openwb.length() > 0) {
              ret += ",\"openwb\": [{\"openwbtopic\": \"" + OpenWB->getOpenWbTopic(this->InverterLiveData->at(i).openwb) + "\"}]";
            }
            ret += "}";
            (*firstRow) = false;
          } 

          (*counter)++;
          i++;
        }

      }
      
      if (this->InverterIdData->size() + this->InverterLiveData->size() + 1 == *counter) {
        // send end of JSON
        ret += " ]}, \"object_id\": \"" + Config->GetMqttBasePath() + "/" + Config->GetMqttRoot() + "\"}";
        (*counter)++;
      }
      int len = sprintf((char*)buffer, ret.c_str());
      return len;

	});

	request->send(response);
}

/*******************************************************
 * Return all LiveData as jsonArray
 * {data: [{"name": "xx", "value": "xx", ...}, ...] }
*******************************************************/
void modbus::GetSettersAsJsonToWebServer(AsyncWebServerRequest *request) {
  std::shared_ptr<uint16_t> counter = std::make_shared<uint16_t>(0);
  String subaction("");
  
  if(request->hasArg("json")) {
    const String json = request->arg("json");
    Config->logN(4, "[GetSetterAsJson] Json command empfangen: %s", json.c_str());

    JsonDocument jsonGet; 
    DeserializationError error = deserializeJson(jsonGet, json.c_str());
    
    if (!error) {
      if (jsonGet["cmd"]["subaction"]) subaction = jsonGet["cmd"]["subaction"].as<String>();
    } else { 
      Config->logN(2, "[GetSetterAsJson] Json Command not parseable: %s -> %s", json.c_str(), error.c_str());
    }
  }

  AsyncWebServerResponse *response = request->beginChunkedResponse("application/json", [this, counter, subaction](uint8_t *buffer, size_t maxLen, size_t index) {
			String ret("");
      ret.reserve(maxLen);
      maxLen -= 500; // use a puffer of 500 bytes, every item is assumed to be 200 bytes

      if (*counter == 0) {
        // send start of JSON
        ret += "{\"globalEnabled\": \""+ String(this->Conf_EnableSetters) +"\",  \"data\": {\"setitems\": [";
        (*counter)++;
      }

      File regfile = LittleFS.open("/regs/"+this->InverterType.filename);
      if (!regfile) {
        Config->logN(1, "failed to open %s file", this->InverterType.filename.c_str());
        return 0;
      }

      String streamString = "";
      uint16_t itemIterator = 0;

      streamString = "\""+ this->InverterType.name +"\": {";
      regfile.find(streamString.c_str());
 
      streamString = "\"set\": [";
      regfile.find(streamString.c_str());
      do {
        if (itemIterator == (*counter - 1)) {
          bool isActive = false;  // default
          JsonDocument elem;
          DeserializationError error = deserializeJson(elem, regfile); 
  
          if (error) {
            Config->logN(1, "(Function GetSettersAsJsonToWebServer) Failed to parse JSON Register Data: %s", error.c_str()); 
            break;
          }

          Config->logN(4, "parsing JSON ok");
          Config->log(5, elem);

          //check if setter is active
          for (uint8_t i = 0; i < this->Setters->size(); i++) {
            if (this->Setters->at(i).Name == elem["name"].as<String>()) {
              isActive = this->Setters->at(i).active;
              break;
            }
          }

          if ((subaction == "onlyactive" && isActive) || subaction != "onlyactive") {
            if(*counter > 1) ret += ",";
            String mapping = elem["mapping"].as<String>(); mapping.replace("\"", "'");
            
            ret += "{\"name\": \"" + elem["name"].as<String>() + "\",";
            
            ret += "\"realname\": {\"innerHTML\": \"" + elem["realname"].as<String>() + "\"";
            if (elem["info"])     ret += ", \"data-info\": \"" + elem["info"].as<String>() + "\"";
            ret += "},";
            
            ret += "\"active\": {\"checked\": " + String(isActive ? 1 : 0) + ", \"name\": \"" + elem["name"].as<String>() + "\"},";
            
            ret += "\"subscription\": {\"innerHTML\": \"" + this->GetMqttSetTopic(elem["name"].as<String>()) + "\"";
            if (elem["mapping"])  ret += ", \"data-mapping\": \""+ mapping + "\"";             
            ret += "}}";
          }
        }

        (*counter)++;
        itemIterator++;

      } while (regfile.findUntil(",","]"));

      if (regfile) { regfile.close(); }

      if (ret.length() > 0) {
        // send end of JSON
        ret += " ]}, \"object_id\": \"" + Config->GetMqttBasePath() + "/" + Config->GetMqttRoot() + "\"}";
        (*counter)++;
      }

      int len = sprintf((char*)buffer, ret.c_str());
      return len;

	});

	request->send(response);
}

/*******************************************************
 * Return all LiveData as jsonArray
 * {data: [{"name": "xx", "value": "xx"}], }
 * {"GridVoltage_R":"0.00 V","GridCurrent_R":"0.00 A","GridPower_R":"0 W","GridFrequency_R":"0.00 Hz","GridVoltage_S":"0.90 V","GridCurrent_S":"1715.40 A","GridPower_S":"-28671 W","GridFrequency_S":"174.08 Hz","GridVoltage_T":"0.00 V","GridCurrent_T":"0.00 A","GridPower_T":"0 W","GridFrequency_T":"1.30 Hz","PvVoltage1":"259.80 V","PvVoltage2":"0.00 V","PvCurrent1":"1.00 A","PvCurrent2":"0.00 A","Temperature":"28 &deg;C","PowerPv1":"283 W","PowerPv2":"0 W","BatVoltage":"0.00 V","BatCurrent":"0.00 A","BatPower":"0 W","BatTemp":"0 &deg;C","BatCapacity":"0 %","OutputEnergyChargeWh":"0 Wh","OutputEnergyChargeKWh":"0.00 KWh","OutputEnergyChargeToday":"0.00 KWh","InputEnergyChargeWh":"0 Wh","InputEnergyChargeKWh":"0.00 KWh"}
*******************************************************/
/*
void modbus::GetRegisterAsJsonToWebServer(AsyncResponseStream *response) {
  int count = 0;
  
  File regfile = LittleFS.open("/regs/"+this->InverterType.filename);
  if (!regfile) {
    Config->logN(1, "failed to open %s file", this->InverterType.filename.c_str());
    return;
  }

  response->print("{\"data\": ["); 
  
  String streamString = "";
  streamString = "\""+ this->InverterType.name +"\": {";
  regfile.find(streamString.c_str());

  streamString = "\"livedata\": [";
  regfile.find(streamString.c_str());

  do {
    JsonDocument elem;
    DeserializationError error = deserializeJson(elem, regfile); 
      
    if (!error) {
      // Print the result
      Config->logN(4, "parsing JSON ok"); 
      Config->logN(5, elem);
    } else {
      Config->logN(4, "(Function GetRegisterAsJson) Failed to parse JSON Register Data: %s", error.c_str()); 
    }
  
    String s = "";
    serializeJson(elem, s);
    if(count>0) response->print(", ");
    response->print(s);
    count++;

  } while (regfile.findUntil(",","]"));
  //Lazgar
  streamString = "";
  streamString = "\""+ this->InverterType.name +"\": {";
  regfile.find(streamString.c_str());

  streamString = "\"id\": [";
  regfile.find(streamString.c_str());

  do {
    JsonDocument elem;
    DeserializationError error = deserializeJson(elem, regfile); 

    if (!error) {
      // Print the result
      Config->logN(4, "parsing JSON ok"); 
      Config->logN(5, elem);
    } else {
      Config->logN(1, "(Function GetRegisterAsJson) Failed to parse JSON Register Data: %s", error.c_str()); 
    }

    String s = "";
    serializeJson(elem, s);
    if(count>0) response->print(", ");
    response->print(s);
    count++;

  } while (regfile.findUntil(",","]"));
  //Lazgar
  if (regfile) { regfile.close(); }
  response->print("]}");
}
*/

/*******************************************************
 * request for changing active Status of a certain item
 * Used by handleAjax function
*******************************************************/
void modbus::SetItemActiveStatus(String item, bool newstate) {
  for (uint16_t j=0; j < this->InverterLiveData->size(); j++) {
    if (this->InverterLiveData->at(j).Name == item) {
      Config->logN(3, "Set Item <%s> ActiveState to %s", item.c_str(), (newstate?"true":"false"));
      this->InverterLiveData->at(j).active = newstate;
      return;
    }
  }
  
  for (uint16_t j=0; j < this->InverterIdData->size(); j++) {
    if (this->InverterIdData->at(j).Name == item) {
      Config->logN(3, "Set Item <%s> ActiveState to %s", item.c_str(), (newstate?"true":"false"));
      this->InverterIdData->at(j).active = newstate;
      return;
    }
  }

  for (uint16_t j=0; j < this->Setters->size(); j++) {
    if (this->Setters->at(j).Name == item) {
      Config->logN(3, "Set Item <%s> ActiveState to %s", item.c_str(), (newstate?"true":"false"));
      if (this->mqtt && this->Setters->at(j).active != newstate) {
	      if (!newstate) {
          this->mqtt->UnSubscribe(this->GetMqttSetTopic(this->Setters->at(j).Name));
        } else {
          this->mqtt->Subscribe(this->GetMqttSetTopic(this->Setters->at(j).Name));
	      }
      }
      this->Setters->at(j).active = newstate;
      return;
    }
  }

}

/*******************************************************
 * loop function
*******************************************************/
void modbus::loop() {
  // handle requesting ID Data into queue
  if (millis() - this->LastTxIdData > this->TxIntervalIdData * 1000) {
    
    
    if (this->InverterType.filename.length() > 1) {this->QueryIdData();}
  }

  // handle requesting LiveData into queue
  if (millis() - this->LastTxLiveData > this->TxIntervalLiveData * 1000) {
    
    
    if (this->InverterType.filename.length() > 1) {
      this->QueryLiveData();
      if (this->enableRelays) this->ReadRelays();
    }  
   }

  //its allowed to send a new request every 800ms, we use recommended 1000ms
  if (millis() - this->LastTxInverter > 1000) {
    this->LastTxInverter = millis();

    if (this->InverterType.filename.length() > 1) {this->QueryQueueToInverter();}
  }
}

/*******************************************************
 * load initial Register Items from file into vector
*******************************************************/
void modbus::LoadRegItems(std::vector<reg_t>* vector, String type) {
  vector->clear();

  Config->logN(4, "Load RegItems for Inverter %s and type <%s>", this->InverterType.name.c_str(), type.c_str());

  File regfile = LittleFS.open("/regs/"+this->InverterType.filename);
  if (!regfile) {
    Config->logN(1, "failed to open %s file", this->InverterType.filename.c_str());
    return;
  }

  String streamString = "";
  streamString = "\""+ this->InverterType.name +"\": {";
  regfile.find(streamString.c_str());
    
  streamString = "\"" + type + "\": [";
  regfile.find(streamString.c_str());
  do {
    JsonDocument elem;
    DeserializationError error = deserializeJson(elem, regfile); 
      
    if (!error) {
      // Print the result
      Config->logN(4, "parsing JSON ok");
      Config->log(5, elem);
    } else {
      Config->logN(1, "(Function LoadRegItems) Failed to parse JSON Register Data for Inverter <%s> and type <%s>: %s", this->InverterType.name.c_str(), type.c_str(), error.c_str());
    }

    reg_t d = {};
      
    // mandantory field
    if(!elem["name"].isNull()) {
      d.Name = elem["name"].as<String>();
    } else {
      d.Name = String("undefined");
    }

    // optional field
    if(!elem["realname"].isNull()) {
      d.RealName = elem["realname"].as<String>();
    } else {
      d.RealName = d.Name;
    }

    // optional field
    if(!elem["openwbtopic"].isNull()) {
      d.openwb = elem["openwbtopic"].as<String>();
    } 

    d.active = false; // set initial
    vector->push_back(d);

    Config->logN(4, "processed RegItem: %s", d.Name.c_str());

  } while (regfile.findUntil(",","]"));

  if (regfile) { regfile.close(); }
}

/*******************************************************
 * load configuration from file
*******************************************************/
void modbus::LoadJsonConfig(bool firstrun) {
  bool loadDefaultConfig = false;
  uint32_t Baudrate_old = this->Baudrate;
  uint8_t pin_RX_old    = this->pin_RX;
  uint8_t pin_TX_old    = this->pin_TX;
  uint8_t pin_RTS_old   = this->pin_RTS;
  regfiles_t InverterType_old = this->InverterType;
  uint8_t pin_Relay1_old   = this->pin_Relay1;
  uint8_t pin_Relay2_old   = this->pin_Relay2;
  bool enableRelays_old  = this->enableRelays;
  bool enableSetters_old = this->Conf_EnableSetters;

  if (LittleFS.exists("/config/modbusconfig.json")) {
    //file exists, reading and loading
    Config->logN(3, "reading config file....");
    File configFile = LittleFS.open("/config/modbusconfig.json", "r");
    if (configFile) {
      Config->logN(3, "config file is open:");
      //size_t size = configFile.size();

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, configFile);
      
      if (!error && doc["data"]) {
        Config->log(3, doc);
        OpenWB->clearMappings();

        if (doc["data"]["pin_rx"])           { this->pin_RX = (int)(doc["data"]["pin_rx"]);} else {this->pin_RX = this->default_pin_RX;}
        if (doc["data"]["pin_tx"])           { this->pin_TX = (int)(doc["data"]["pin_tx"]);} else {this->pin_TX = this->default_pin_TX;}
        if (doc["data"]["pin_rts"])          { this->pin_RTS = (int)(doc["data"]["pin_rts"]);} else {this->pin_RTS = this->default_pin_RTS;}
        if (doc["data"]["clientid"])         { this->ClientID = strtoul(doc["data"]["clientid"], NULL, 16);} else {this->ClientID = 0x01;} // hex convert to dec
        if (doc["data"]["baudrate"])         { this->Baudrate = (int)(doc["data"]["baudrate"]);} else {this->Baudrate = 19200;}
        if (doc["data"]["txintervallive"])   { this->TxIntervalLiveData = (int)(doc["data"]["txintervallive"]);} else {this->TxIntervalLiveData = 5;}
        if (doc["data"]["txintervalid"])     { this->TxIntervalIdData = (int)(doc["data"]["txintervalid"]);} else {this->TxIntervalIdData = 3600;}
        if (doc["data"]["pin_RELAY1"])       { this->pin_Relay1= doc["data"]["pin_RELAY1"].as<int>();} else {this->pin_Relay1 = this->default_pin_Relay1;}
        if (doc["data"]["pin_RELAY2"])       { this->pin_Relay2 = doc["data"]["pin_RELAY2"].as<int>();} else {this->pin_Relay2 = this->default_pin_Relay2;}
        if (doc["data"]["openwbversion"])    { this->Conf_OpenWBVersion = doc["data"]["openwbversion"].as<String>(); this->OpenWB->setVersion(this->Conf_OpenWBVersion); }
        if (doc["data"]["openwbmodulid"])    { this->Conf_OpenWBModulID = doc["data"]["openwbmodulid"].as<uint8_t>(); this->OpenWB->addMapping("InverterID", String(this->Conf_OpenWBModulID)); }
        if (doc["data"]["openwbbatteryid"])  { this->Conf_OpenWBBatteryID = doc["data"]["openwbbatteryid"].as<uint8_t>(); this->OpenWB->addMapping("BatteryID", String(this->Conf_OpenWBBatteryID)); }

        this->Conf_EnableOpenWB       = doc["data"]["enableOpenWb"].as<bool>();
        this->Conf_EnableSetters      = doc["data"]["enable_setters"].as<bool>();
        this->enableCrcCheck          = doc["data"]["enableCrcCheck"].as<bool>();
        this->enableLengthCheck       = doc["data"]["enableLengthCheck"].as<bool>();
        this->enableRelays            = doc["data"]["enableRelays"].as<bool>();
        
        if (doc["data"]["invertertype"])     { 
          bool found = false;
          for (uint8_t i=0; i<this->AvailableInverters->size(); i++) {
            if (this->AvailableInverters->at(i).name == (doc["data"]["invertertype"]).as<String>()) {
              this->InverterType = this->AvailableInverters->at(i); 
              
              Config->logN(3, "Invertertyp '%s' was found in register file '%s', set it as selected active Inverter", this->InverterType.name.c_str(), this->InverterType.filename.c_str());
              found = true;
            } 
          }
          if (!found) { 
            if (this->AvailableInverters->size()>0) {
              this->InverterType = this->AvailableInverters->at(0);
            }
            Config->logN(3, "Invertertyp '%s' was not found, use default '%s' instead", (doc["data"]["invertertype"]).as<String>().c_str(), this->InverterType.name.c_str());
          }
        }

      } else {
        Config->logN(1, "failed to load modbus json config, load default config");
        loadDefaultConfig = true;
      }
      configFile.close();
    }
  } else {
    Config->logN(3, "modbusconfig.json config File not exists, load default config");
    loadDefaultConfig = true;
  }

  if (loadDefaultConfig) {
    if (this->AvailableInverters->size()>0) {
      this->InverterType = this->AvailableInverters->at(0);
    } 

    this->pin_RX = this->default_pin_RX;
    this->pin_TX = this->default_pin_TX;
    this->pin_RTS = this->default_pin_RTS;
    this->ClientID = 0x01;
    this->Baudrate = 19200;
    this->TxIntervalLiveData = 5;
    this->TxIntervalIdData = 3600;
    this->pin_Relay1 = this->default_pin_Relay1;
    this->pin_Relay2 = this->default_pin_Relay2;
    this->enableRelays = false;
    this->Conf_EnableOpenWB = false;
    this->Conf_OpenWBVersion = "1.9";
    this->Conf_EnableSetters = false;

    loadDefaultConfig = false; //set back
  }

  // ReInit if basics has been changed, not at firstrun!
  if(!firstrun && (
     (Baudrate_old != this->Baudrate) ||
     (pin_RX_old   != this->pin_RX)   ||
     (pin_TX_old   != this->pin_TX)   ||
     (pin_RTS_old  != this->pin_RTS)  ||
     (enableRelays_old != this->enableRelays) ||
     (pin_Relay1_old  != this->pin_Relay1)  ||
     (pin_Relay2_old  != this->pin_Relay2) ||
     (InverterType_old.name != this->InverterType.name))) { 
    
    this->init(false);
  }

  if (enableSetters_old != this->Conf_EnableSetters) {
    this->LoadSettersFromRegFile();
    this->LoadJsonItemConfig(false, false, true); // load only Setters
  }

}

/*******************************************************
 * load Modbus Item configuration from file
*******************************************************/
void modbus::LoadJsonItemConfig() { 
  this->LoadJsonItemConfig(true, true, true);
}

void modbus::LoadJsonItemConfig(bool loadLiveData, bool loadIdData, bool loadSetters) {
  
  if (LittleFS.exists("/config/modbusitemconfig.json")) {
    //file exists, reading and loading
    Config->logN(3, "reading modbus item config file....");
    File configFile = LittleFS.open("/config/modbusitemconfig.json", "r");
    if (configFile) {
      Config->logN(3, "modbus item config file is open:");

      ReadBufferingStream stream{configFile, 64};
      stream.find("\"data\":[");
      do {
        JsonDocument elem;
        DeserializationError error = deserializeJson(elem, stream); 

        if (!error) {
          // Print the result
          Config->logN(4, "parsing JSON ok"); 
          Config->log(5, elem);
        } else {
          Config->logN(1, "(Function LoadJsonItemConfig) Failed to parse JSON Register Data: %s", error.c_str()); 
        }

        for (JsonPair kv : elem.as<JsonObject>()) {
          const char* ItemName = kv.key().c_str();
          
          /* handle LiveData */
          if (loadLiveData) {
            for(uint16_t i=0; i<this->InverterLiveData->size(); i++) {
              if (this->InverterLiveData->at(i).Name == ItemName ) {
                this->InverterLiveData->at(i).active = kv.value().as<bool>();

                Config->logN(3, "item %s -> %s", ItemName, (this->InverterLiveData->at(i).active?"enabled":"disabled"));

                break;
              }
            }
          }

          if (loadIdData) {
            /* handle IdData */
            for(uint16_t i=0; i<this->InverterIdData->size(); i++) {
              if (this->InverterIdData->at(i).Name == ItemName ) {
                this->InverterIdData->at(i).active = kv.value().as<bool>();

                Config->logN(3, "item %s -> %s", ItemName, (this->InverterIdData->at(i).active?"enabled":"disabled"));
                break;
              }
            }
          }

          if (loadSetters) {
            /* handle Setters */  
            for(uint16_t i=0; i<this->Setters->size(); i++) {
              if (this->Setters->at(i).Name == ItemName ) {
                this->Setters->at(i).active = kv.value().as<bool>();
                Config->logN(3, "setter %s -> %s", ItemName, (this->Setters->at(i).active?"enabled":"disabled"));
                break;
              }
            }
          }
	  
        }

      } while (stream.findUntil(",","]"));
      configFile.close();
    } else {
      Config->logN(1, "failed to load modbusitemconfig.json, load default item config");
    }
  } else {
    Config->logN(3, "modbusitemconfig.json config File not exists, all items are inactive as default");
  }
}

/*******************************************************************************************************
 * WebContent
*******************************************************************************************************/
void modbus::GetInitData(JsonDocument &json){
  json["data"].to<JsonObject>();
  json["data"]["GpioPin_RX"]          = this->pin_RX;
  json["data"]["GpioPin_TX"]          = this->pin_TX;
  json["data"]["GpioPin_RTS"]         = this->pin_RTS;
  json["data"]["clientid"]            = String(this->ClientID, HEX);
  json["data"]["baudrate"]            = this->Baudrate;
  json["data"]["txintervallive"]      = this->TxIntervalLiveData;
  json["data"]["txintervalid"]        = this->TxIntervalIdData;
  json["data"]["GpioPin_Relay1"]      = this->pin_Relay1;
  json["data"]["GpioPin_Relay2"]      = this->pin_Relay2;
  json["data"]["enableRelays"]        = ((this->enableRelays)?1:0);

  json["data"]["enableOpenWb"]        = ((this->Conf_EnableOpenWB)?1:0);
  json["data"]["openwbmodulid"]       = this->Conf_OpenWBModulID;
  json["data"]["openwbbatteryid"]     = this->Conf_OpenWBBatteryID;

  json["data"]["enableCrcCheck"]      = ((this->enableCrcCheck)?1:0);
  json["data"]["enableLengthCheck"]   = ((this->enableLengthCheck)?1:0);
  json["data"]["enable_setters"]      = ((this->Conf_EnableSetters)?1:0);
  
  for (uint8_t i=0; i< AvailableInverters->size(); i++) {
    json["data"]["inverters"][i]["inverter"].to<JsonObject>();
    json["data"]["inverters"][i]["inverter"]["value"] = AvailableInverters->at(i).name;
    json["data"]["inverters"][i]["inverter"]["selected"] = (AvailableInverters->at(i).name == this->InverterType.name?1:0);
    json["data"]["inverters"][i]["inverter"]["text"] = AvailableInverters->at(i).name;
  }

  const std::vector<String> *OpenWBVersions = OpenWB->getOpenWbVersions();

  for (uint8_t i=0; i< OpenWBVersions->size(); i++) {
    json["data"]["openwbversions"][i]["openwbversion"].to<JsonObject>();
    json["data"]["openwbversions"][i]["openwbversion"]["value"] = OpenWBVersions->at(i);
    json["data"]["openwbversions"][i]["openwbversion"]["selected"] = (OpenWBVersions->at(i) == this->Conf_OpenWBVersion?1:0);
    json["data"]["openwbversions"][i]["openwbversion"]["text"] = OpenWBVersions->at(i);
  }

  json["response"].to<JsonObject>();
  json["response"]["status"] = 1;
  json["response"]["text"] = "successful";
}

void modbus::GetInitRawData(JsonDocument& json) {
  std::ostringstream id, live;
  
  live << std::hex << std::uppercase;
  id << std::hex << std::uppercase;

  for (uint16_t i = 0; i < this->SaveIdDataframe->size(); i++) {
    id << std::setw(2) << std::setfill('0') << (int)this->SaveIdDataframe->at(i);
  }

  for (uint16_t i = 0; i < this->SaveLiveDataframe->size(); i++) {
    live << std::setw(2) << std::setfill('0') << (int)this->SaveLiveDataframe->at(i);
  }

  json["data"].to<JsonObject>();
  json["data"]["id_rawdata_org"] = id.str();
  json["data"]["live_rawdata_org"] = live.str();

  json["response"].to<JsonObject>();
  json["response"]["status"] = 1;
  json["response"]["text"] = "successful";
}
