/*Program to use GATT service on ESP32 to send Battery Level
 * ESP32 works as server - Mobile as client
 * Program by: B.Aswinth Raj
 * Dated on: 13-10-2018
 * Website: www.circuitdigest.com
 */

#include <Arduino.h>
#include <Preferences.h>
#define WM_MDNS true
#include <WiFiManager.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
/*
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <esp_log.h>
*/

String _deviceName;
String _networkClient;
int _portNumber;
int _baudRate;
bool _portNumberChanged = true;
Preferences preferences;
const int _xmitLedPin = 2;

const char *getParameterValue(const char *id);
void _saveConfigCallback();
void _saveParamsCallback();
void setPrefs(const char *name, const char *port, const char *server);
bool _connectNetworkClient(const char *client_address, int client_port);
bool _startListenServer();
void _clientConnectedListenCallback(void *args, AsyncClient *theClient);
void _clientConnectedCallback();
void _onError(int8_t error);
void _onData(void *buf, size_t len);
void _onPoll();
void _onAck(size_t len, uint32_t time);
void _onTimeout(uint32_t time);
void _onDisconnect();
void sendDataToNetwork(const char *buf, size_t len);

// Initialize the Connection for the RS422 interface
#define EX_UART_NUM UART_NUM_1
#define EX_UART_TXD (GPIO_NUM_4)
#define EX_UART_RXD (GPIO_NUM_5)
#define EX_UART_RTS (UART_PIN_NO_CHANGE)
#define EX_UART_CTS (UART_PIN_NO_CHANGE)
#define PATTERN_CHR_NUM (3) /*!< Set the number of consecutive and identical characters received by receiver which defines a UART pattern*/

#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)
static QueueHandle_t uart0_queue;
uart_config_t uart_config;

// WiFiManager
WiFiManager *_wifiManager;

bool _wifiConnected = false;

// Initialize the client library
AsyncServer *_listenServer;
AsyncClient *_clientNetwork;
size_t _ackedLength;
size_t _writtenLength;
typedef enum
{
  DATA_SETUP,
  DATA_CONTENT,
  DATA_WAIT_ACK,
  DATA_END,
  DATA_FAILED
} SendDataState;

SendDataState _state;
int _network_read_bytes = 0;
int _network_write_bytes = 0;
bool _netClientConnected = false;
bool _connectClient = false;

/*
 * Data Stream (You can print/write/printf to it, up to the contentLen bytes)
 * */

class cbuf;

class AsyncDataStream
{
public:
  AsyncDataStream(size_t bufferSize);
  ~AsyncDataStream();
  size_t fillBuffer(char *buf, size_t maxLen);
  size_t write(const char *data, size_t len);
  size_t write(char data);
  size_t getLength();

private:
  cbuf *_content;
  size_t _contentLength;
};

AsyncDataStream::AsyncDataStream(size_t bufferSize)
{
  _content = new cbuf(bufferSize);
  _contentLength = 0;
}

AsyncDataStream::~AsyncDataStream()
{
  delete _content;
}

size_t AsyncDataStream::fillBuffer(char *buf, size_t maxLen)
{
  size_t readLength = _content->read(buf, maxLen);
  _contentLength -= readLength;
  return readLength;
}

size_t AsyncDataStream::write(const char *data, size_t len)
{
  if (len > _content->room())
  {
    size_t needed = len - _content->room();
    _content->resizeAdd(needed);
  }
  size_t written = _content->write(data, len);
  _contentLength += written;
  return written;
}

size_t AsyncDataStream::write(char data)
{
  return write(&data, 1);
}

size_t AsyncDataStream::getLength()
{
  return _contentLength;
}

AsyncDataStream *_dataStream;

void uart_event_task(void *pvParameters)
{
  uart_event_t event;
  size_t buffered_size;
  uint8_t *dtmp = (uint8_t *)malloc(RD_BUF_SIZE);
  int pos;
  for (;;)
  {
    //Waiting for UART event.
    if (xQueueReceive(uart0_queue, (void *)&event, (portTickType)portMAX_DELAY))
    {
      bzero(dtmp, RD_BUF_SIZE);
      // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "uart[%d] event:", EX_UART_NUM);
      switch (event.type)
      {
      //Event of UART receving data
      /*We'd better handler data event fast, there would be much more data events than
                other types of events. If we take too much time on data event, the queue might
                be full.*/
      case UART_DATA:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "[UART DATA]: %d", event.size);
        digitalWrite(_xmitLedPin, HIGH);
        uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "[DATA EVT]:");
        // uart_write_bytes(EX_UART_NUM, (const char*) dtmp, event.size);
        sendDataToNetwork((const char *)dtmp, event.size);
        digitalWrite(_xmitLedPin, LOW);
        break;
      //Event of HW FIFO overflow detected
      case UART_FIFO_OVF:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "hw fifo overflow");
        // If fifo overflow happened, you should consider adding flow control for your application.
        // The ISR has already reset the rx FIFO,
        // As an example, we directly flush the rx buffer here in order to read more data.
        uart_flush_input(EX_UART_NUM);
        xQueueReset(uart0_queue);
        break;
      //Event of UART ring buffer full
      case UART_BUFFER_FULL:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "ring buffer full");
        // If buffer full happened, you should consider encreasing your buffer size
        // As an example, we directly flush the rx buffer here in order to read more data.
        uart_flush_input(EX_UART_NUM);
        xQueueReset(uart0_queue);
        break;
      //Event of UART RX break detected
      case UART_BREAK:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "uart rx break");
        break;
      //Event of UART parity check error
      case UART_PARITY_ERR:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "uart parity error");
        break;
      //Event of UART frame error
      case UART_FRAME_ERR:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "uart frame error");
        break;
      //UART_PATTERN_DET
      case UART_PATTERN_DET:
        uart_get_buffered_data_len(EX_UART_NUM, &buffered_size);
        pos = uart_pattern_pop_pos(EX_UART_NUM);
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "[UART PATTERN DETECTED] pos: %d, buffered size: %d", pos, buffered_size);
        if (pos == -1)
        {
          // There used to be a UART_PATTERN_DET event, but the pattern position queue is full so that it can not
          // record the position. We should set a larger queue size.
          // As an example, we directly flush the rx buffer here.
          uart_flush_input(EX_UART_NUM);
        }
        else
        {
          digitalWrite(_xmitLedPin, HIGH);
          uart_read_bytes(EX_UART_NUM, dtmp, pos, 100 / portTICK_PERIOD_MS);
          uint8_t pat[PATTERN_CHR_NUM + 1];
          memset(pat, 0, sizeof(pat));
          sendDataToNetwork((const char *)dtmp, event.size);
          digitalWrite(_xmitLedPin, LOW);
          // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "read data: %s", dtmp);
          // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "read pat : %s", pat);
        }
        break;
      //Others
      default:
        // ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, TAG_UART, "uart event type: %d", event.type);
        break;
      }
    }
  }
  free(dtmp);
  dtmp = NULL;
  vTaskDelete(NULL);
}

void decodeEspError(const char *functionName, esp_err_t errorCode)
{
  if (errorCode != ESP_OK)
  {
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, TAG_UART, "%s error was received with error no: %s", String(functionName).c_str(), esp_err_to_name(errorCode));
    Serial.printf("%s error was received with error no: %s", String(functionName).c_str(), esp_err_to_name(errorCode));
    delay(10000);
    ESP.restart();
  }
}

void setup()
{
  pinMode(_xmitLedPin, OUTPUT);
  digitalWrite(_xmitLedPin, LOW);
#ifdef ESP32
  long efuseMac = ESP.getEfuseMac();
  if (efuseMac < 0)
    efuseMac = efuseMac * (-1);
  _deviceName = String("RS422WIFI_" + String(efuseMac));
#elif defined(ESP8266)
  int chip_id = ESP.getChipId();
  _deviceName = String("RS422WIFI_" + String(chip_id));
#endif
  Serial.begin(115200);
  if (!preferences.begin("rs422espwifi", true))
  {
    if (preferences.begin("rs422espwifi", false))
    {
      preferences.putInt("rs422.rate", 9600);
      preferences.putInt("rs422.port", 9999);
      preferences.putString("rs422.client", "");
    }
  }

  _baudRate = preferences.getInt("rs422.rate", 9600);
  _portNumber = preferences.getInt("rs422.port", 9999);
  _networkClient = preferences.getString("rs422.client");
  preferences.end();

  if (_networkClient.length() > 0)
  {
    _connectClient = true;
  }
  else
  {
    _connectClient = false;
  }
  _listenServer = NULL;

  Serial.println(_deviceName + " RS422 to Wifi");

  _wifiManager = new WiFiManager();
  //  _wifiManager->setEspLogLevel(ESP_LOG_VERBOSE);
  char port_string[10];
  sprintf(port_string, "%d", _portNumber);
  char rate_string[10];
  sprintf(rate_string, "%d", _baudRate);
  //add all your parameters here
  //_wifiManager->addParameter(new WiFiManagerParameter("log_level", "Log Level (0-5)", "0", 1, "type=\"tel\"", WFM_LABEL_BEFORE));
  //_wifiManager->addParameter(new WiFiManagerParameter("log_dest", "Log )Dest (IP:Port | blank)", "", 40, "", WFM_LABEL_BEFORE));
  _wifiManager->addParameter(new WiFiManagerParameter("network_server", "Network Server", _networkClient.c_str(), 40, "type=\"tel\"", WFM_LABEL_BEFORE));
  _wifiManager->addParameter(new WiFiManagerParameter("network_port", "Network Port", port_string, 5, "type=\"tel\"", WFM_LABEL_BEFORE));
  _wifiManager->addParameter(new WiFiManagerParameter("baud_rate", "Baud Rate", rate_string, 7, "ype=\"tel\"", WFM_LABEL_BEFORE));
  //set config save notify callback
  _wifiManager->setSaveConfigCallback(_saveConfigCallback);
  _wifiManager->setSaveParamsCallback(_saveParamsCallback);
  _wifiManager->setConfigPortalBlocking(false);
  if (_wifiManager->autoConnect(_deviceName.c_str(), "networkdev1."))
  {
    Serial.println("Wifi Connected to router");
    _wifiConnected = true;
    _wifiManager->startConfigPortal(_deviceName.c_str(), "networkdev1.");
    if (_networkClient.length() > 0)
    {
      _startListenServer();
    }
  }
  else
  {
    Serial.println("Portal Running");
  }
  /* Configure parameters of an UART driver,
         * communication pins and install the driver */
  uart_config = {
      .baud_rate = atoi(getParameterValue("baud_rate")),
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

  esp_err_t the_error;
  the_error = uart_param_config(EX_UART_NUM, &uart_config);
  decodeEspError("uart_param_config", the_error);

  //Set UART pins (using UART0 default pins ie no changes.)
  the_error = uart_set_pin(EX_UART_NUM, EX_UART_TXD, EX_UART_RXD, EX_UART_RTS, EX_UART_CTS);
  decodeEspError("uart_set_pin", the_error);

  //Install UART driver, and get the queue.
  the_error = uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
  decodeEspError("uart_driver_install", the_error);

  //Set uart pattern detect function.
  the_error = uart_enable_pattern_det_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 10000, 10, 10);
  decodeEspError("uart_enable_pattern_det_intr", the_error);

  //Reset the pattern queue length to record at most 20 pattern positions.
  the_error = uart_pattern_queue_reset(EX_UART_NUM, 20);
  decodeEspError("uart_pattern_queue_reset", the_error);

  //Create a task to handler UART event from ISR
  xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 12, NULL);
}

void loop()
{
  _wifiManager->process();
  if (_connectClient)
  {
    if (_connectNetworkClient(getParameterValue("network_server"), atoi(getParameterValue("network_port"))))
    {
      _connectClient = false;
      _netClientConnected = false;
    }
    else
    {
      // ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, TAG_ENF, "Network server could not connect Ip (%s) Retrying in 10 secs...", getParameterValue("network_server"));
      delay(10000);
    }
    delay(10);
  }
}

const char *getParameterValue(const char *id)
{
  WiFiManagerParameter **theParameters = _wifiManager->getParameters();
  int theCount = _wifiManager->getParametersCount();

  for (int i = 0; i < theCount; i++)
  {
    if (strcmp(id, theParameters[i]->getID()) == 0)
    {
      return theParameters[i]->getValue();
    }
  }
  return NULL;
}

//callback notifying us of the need to save config
void _saveConfigCallback()
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, TAG_ENF, "Should save config");
  Serial.println("Config callback triggered.");
  _wifiManager->startConfigPortal(_deviceName.c_str(), "networkdev1.");
  setPrefs(getParameterValue("baud_rate"), getParameterValue("network_port"), getParameterValue("network_server"));
  if (_networkClient.length() > 0)
  {
    _startListenServer();
  }
}

//callback notifying us of the need to save params
void _saveParamsCallback()
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, TAG_ENF, "Should save config");
  Serial.println("Params callback triggered.");
  int old_portNum = _portNumber;
  String old_client = String(_networkClient);
  setPrefs(getParameterValue("baud_rate"), getParameterValue("network_port"), getParameterValue("network_server"));
  if (old_portNum != _portNumber || old_client.compareTo(_networkClient))
  {
    if (_netClientConnected)
    {
      _clientNetwork->close();
    }

    _netClientConnected = false;
    if (_listenServer != NULL)
    {
      _listenServer->end();
      _listenServer = NULL;
    }
    delay(100);
    if (_networkClient.length() > 0)
    {
      _connectClient = true;
    }
    else
    {
      _startListenServer();
      _connectClient = false;
    }
  }
}

void setPrefs(const char *baud_rate, const char *port, const char *server)
{
  _baudRate = atoi(baud_rate);
  _portNumber = atoi(port);
  _networkClient = String(server);
  if (preferences.begin("rs422espwifi", false))
  {
    preferences.putInt("rs422.rate", _baudRate);
    preferences.putInt("rs422.port", _portNumber);
    preferences.putString("r422.client", _networkClient);
  }

  preferences.end();
}

bool _startListenServer()
{
  _listenServer = new AsyncServer(_portNumber);
  _listenServer->setNoDelay(true);
  _listenServer->onClient([](void *r, AsyncClient *c) { _clientConnectedListenCallback(r, c); }, NULL);
  _listenServer->begin();

  return true;
}

bool _connectNetworkClient(const char *client_address, int client_port)
{
  // start a server
  IPAddress networkServer;
  if (networkServer.fromString(client_address))
  {
    _clientNetwork = new AsyncClient();
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onConnect");
    _clientNetwork->onConnect([](void *r, AsyncClient *c) { _clientConnectedCallback(); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onError");
    _clientNetwork->onError([](void *r, AsyncClient *c, int8_t error) { _onError(error); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onAck");
    _clientNetwork->onAck([](void *r, AsyncClient *c, size_t len, uint32_t time) { _onAck(len, time); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onDisconnect");
    _clientNetwork->onDisconnect([](void *r, AsyncClient *c) { _onDisconnect(); delete c; }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onTimeout");
    _clientNetwork->onTimeout([](void *r, AsyncClient *c, uint32_t time) { _onTimeout(time); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onData");
    _clientNetwork->onData([](void *r, AsyncClient *c, void *buf, size_t len) { _onData(buf, len); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onPoll");
    _clientNetwork->onPoll([](void *r, AsyncClient *c) { _onPoll(); }, NULL);

    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork do connect");
    if (_clientNetwork->connect(networkServer, client_port))
    {
      // ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, TAG_ENF, "Client to network connect called, wait for connect...");
      yield();
      return true;
    }
    else
    {
      // ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, TAG_ENF, "Client to network not connected (%s)", client_address);
    }
  }
  else
  {
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, TAG_ENF, "Client could not get IPAddress from (%s)", client_address);
  }
  return false;
}

void _clientConnectedListenCallback(void *args, AsyncClient *theClient)
{
  if (!_netClientConnected)
  {
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, TAG_ENF, "Client to network connected - Configured network connection (%s)", _clientNetwork->remoteIP().toString().c_str());
    _netClientConnected = true;
    _connectClient = false;
    _clientNetwork = theClient;
    _clientNetwork->onError([](void *r, AsyncClient *c, int8_t error) { _onError(error); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onAck");
    _clientNetwork->onAck([](void *r, AsyncClient *c, size_t len, uint32_t time) { _onAck(len, time); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onDisconnect");
    _clientNetwork->onDisconnect([](void *r, AsyncClient *c) { _onDisconnect(); delete c; }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onTimeout");
    _clientNetwork->onTimeout([](void *r, AsyncClient *c, uint32_t time) { _onTimeout(time); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onData");
    _clientNetwork->onData([](void *r, AsyncClient *c, void *buf, size_t len) { _onData(buf, len); }, NULL);
    // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "clientNetwork set onPoll");
    _clientNetwork->onPoll([](void *r, AsyncClient *c) { _onPoll(); }, NULL);
  }
  else
  {
    theClient->close();
  }
}

//callback notifying us of the need to save config
void _clientConnectedCallback()
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, TAG_ENF, "Client to network connected - Configured network connection (%s)", _clientNetwork->remoteIP().toString().c_str());
  _netClientConnected = true;
  _connectClient = false;
}

void _onError(int8_t error)
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, TAG_ENF, "Client network (%s:%s) recvd error of %s", getParameterValue("network_server"), getParameterValue("network_port"), _clientNetwork->errorToString(error));
  _connectClient = true;
  _netClientConnected = false;
  delay(3000);
}

void _onData(void *buf, size_t len)
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "client network recvd data for length of %d", len);
  digitalWrite(_xmitLedPin, HIGH);
  // TODO: Add read data stuff
  // uart_write_bytes(EX_UART_NUM, (const char *)buf, len);
  _network_read_bytes += len;
  digitalWrite(_xmitLedPin, LOW);
}

void _onPoll()
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "client network recvd poll");
  _onAck(0, 0);
}

void _onAck(size_t len, uint32_t time)
{
  size_t writeLength = 0;
  size_t lengthToWrite = 0;
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, TAG_ENF, "client network recvd ack of %d", len);
  _ackedLength += len;
  size_t space = _clientNetwork->space();
  size_t outLen = _dataStream->getLength();

  if (_state == DATA_CONTENT)
  {
    if (space >= outLen)
    {
      lengthToWrite = outLen;
      _state = DATA_WAIT_ACK;
    }
    else if (space)
    {
      lengthToWrite = space;
      _state = DATA_CONTENT;
    }
    else
    {
      _state = DATA_CONTENT;
    }

    if (lengthToWrite > 0)
    {
      char *theBuf = (char *)malloc(lengthToWrite * sizeof(char));
      size_t readLength = _dataStream->fillBuffer(theBuf, lengthToWrite);
      writeLength = _clientNetwork->write(theBuf, readLength);
      _writtenLength += writeLength;
      free(theBuf);
      _network_write_bytes += writeLength;
    }
  }
  else if (_state == DATA_WAIT_ACK)
  {
    if (_ackedLength >= _writtenLength)
    {
      _state = DATA_END;
      _writtenLength = 0;
      _ackedLength = 0;
    }
  }
}

void _onTimeout(uint32_t time)
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, TAG_ENF, "Client network timeout, setup retry..");
  _connectClient = true;
  _netClientConnected = false;

  delay(3000);
}

void _onDisconnect()
{
  // ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, TAG_ENF, "Client network recvd disconnect, setup retry..");
  _connectClient = true;
  _netClientConnected = false;
  delay(3000);
}

void sendDataToNetwork(const char *buf, size_t len)
{
  size_t outLen = 0;
  size_t writeLength = 0;
  size_t lengthToWrite = 0;
  size_t space = _clientNetwork->space();

  if (_dataStream == NULL)
    _dataStream = new AsyncDataStream(len);

  _dataStream->write(buf, len);
  outLen = _dataStream->getLength();

  if (space >= outLen)
  {
    lengthToWrite = outLen;
    _state = DATA_WAIT_ACK;
  }
  else if (space)
  {
    lengthToWrite = space;
    _state = DATA_CONTENT;
  }
  else
  {
    _state = DATA_CONTENT;
  }

  if (lengthToWrite > 0)
  {
    char *theBuf = (char *)malloc(lengthToWrite * sizeof(char));
    size_t readLength = _dataStream->fillBuffer(theBuf, lengthToWrite);
    writeLength = _clientNetwork->write(theBuf, readLength);
    _writtenLength += writeLength;
    free(theBuf);
    _network_write_bytes += writeLength;
  }
}
