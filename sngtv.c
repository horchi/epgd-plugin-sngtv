/*
 * sngtv.c
 *
 * epgd plugin for the sngtv EPG JSON API
 *
 * The API is accessed anonymously (guest session), see README for
 * the legal notes - especially regarding the pictures!
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <time.h>
#include <unistd.h>

#include <sstream>
#include <vector>

#include "jpegscale.h"

#include "sngtv.h"

static const char* apiBase = "https://api.prod.sngtv.magentatv.de/EPG/JSON";

xsltStylesheetPtr Sngtv::pxsltStylesheet {};

//***************************************************************************
// Helper
//***************************************************************************

static std::string xmlEscape(const char* s)
{
   std::string out;

   for (; s && *s; s++)
   {
      switch (*s)
      {
         case '<':  out += "&lt;";   break;
         case '>':  out += "&gt;";   break;
         case '&':  out += "&amp;";  break;
         case '"':  out += "&quot;"; break;
         default:
            if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t')
               out += *s;
      }
   }

   return out;
}

static const char* jStr(json_t* obj, const char* key)
{
   json_t* v = json_object_get(obj, key);
   return json_is_string(v) ? json_string_value(v) : nullptr;
}

static void addElement(std::string& xml, const char* name, const char* value)
{
   if (!isEmpty(value))
      xml += std::string("  <") + name + ">" + xmlEscape(value) + "</" + name + ">\n";
}

// API times are like "2026-11-17 14:00:00 UTC+00:00"

static time_t parseUtc(const char* s)
{
   struct tm tm {};

   if (!s || !strptime(s, "%Y-%m-%d %H:%M:%S", &tm))
      return 0;

   return timegm(&tm);
}

// begin of the local day 'day' days from now, formatted for the API (UTC)

static std::string apiTime(int day, int addDays = 0)
{
   char buf[20];
   time_t now = time(nullptr);
   struct tm tm {};

   localtime_r(&now, &tm);
   tm.tm_mday += day + addDays;
   tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
   tm.tm_isdst = -1;

   time_t t = mktime(&tm);
   gmtime_r(&t, &tm);
   strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm);

   return buf;
}

// local date 'day' days from now as YYYYMMDD

static std::string localDate(int day)
{
   char buf[10];
   time_t now = time(nullptr);
   struct tm tm {};

   localtime_r(&now, &tm);
   tm.tm_mday += day;
   tm.tm_hour = 12;
   tm.tm_isdst = -1;
   mktime(&tm);
   strftime(buf, sizeof(buf), "%Y%m%d", &tm);

   return buf;
}

//***************************************************************************
// Object
//***************************************************************************

Sngtv::Sngtv()
{
}

Sngtv::~Sngtv()
{
   exitDb();

   if (handle)
      curl_easy_cleanup(handle);

   if (pxsltStylesheet)
      xsltFreeStylesheet(pxsltStylesheet);

   pxsltStylesheet = nullptr;
}

int Sngtv::init(cEpgd* aObject, int aUtf8)
{
   Plugin::init(aObject, aUtf8);

   if (!pxsltStylesheet)
      pxsltStylesheet = loadXSLT(getSource(), confDir, utf8);

   return done;
}

//***************************************************************************
// Config
//***************************************************************************

int Sngtv::atConfigItem(const char* Name, const char* Value)
{
   if      (!strcasecmp(Name, "images"))        images = atoi(Value) == 1;
   else if (!strcasecmp(Name, "imageType"))     imageType = atoi(Value);
   else if (!strcasecmp(Name, "imageMaxWidth"))  imageMaxWidth = std::max(0, atoi(Value));
   else if (!strcasecmp(Name, "imageMaxHeight")) imageMaxHeight = std::max(0, atoi(Value));
   else if (!strcasecmp(Name, "imageQuality"))   imageQuality = std::min(100, std::max(10, atoi(Value)));
   else if (!strcasecmp(Name, "requestDelay"))  requestDelay = std::max(0, atoi(Value));
   else if (!strcasecmp(Name, "timeout"))       timeout = std::max(5, atoi(Value));
   else if (!strcasecmp(Name, "saveJson"))      saveJson = atoi(Value) == 1;
   else if (!strcasecmp(Name, "saveXml"))       saveXml = atoi(Value) == 1;
   else
      return fail;

   if (!strcasecmp(Name, "images") && images)
      tell(eloWarning, "sngtv: Picture download ENABLED - the pictures are licensed third-party content, "
           "use them for private purposes only (see README of the sngtv plugin)");

   return success;
}

//***************************************************************************
// Init / Exit Database
//***************************************************************************

int Sngtv::initDb()
{
   int status {success};

   // select distinct extid from channelmap where source = ?

   selectDistBySource = new cDbStatement(obj->mapDb);
   selectDistBySource->build("select ");
   selectDistBySource->bind("ExternalId", cDBS::bndOut, "distinct ");
   selectDistBySource->build(" from %s where ", obj->mapDb->TableName());
   selectDistBySource->bind("Source", cDBS::bndIn | cDBS::bndSet);
   status += selectDistBySource->prepare();

   // select channelid, mergesp, merge from channelmap where source = ? and extid = ?

   selectId = new cDbStatement(obj->mapDb);
   selectId->build("select ");
   selectId->bind("ChannelId", cDBS::bndOut);
   selectId->bind("MergeSp", cDBS::bndOut, ", ");
   selectId->bind("Merge", cDBS::bndOut, ", ");
   selectId->build(" from %s where ", obj->mapDb->TableName());
   selectId->bind("Source", cDBS::bndIn | cDBS::bndSet);
   selectId->bind("ExternalId", cDBS::bndIn | cDBS::bndSet, " and ");
   status += selectId->prepare();

   // select tag from fileref where name = ?

   selectByName = new cDbStatement(obj->fileDb);
   selectByName->build("select ");
   selectByName->bind("Tag", cDBS::bndOut);
   selectByName->build(" from %s where ", obj->fileDb->TableName());
   selectByName->bind("Name", cDBS::bndIn | cDBS::bndSet);
   status += selectByName->prepare();

   // mark events of vanished filerefs as deleted

   stmtMarkOldEvents = new cDbStatement(obj->eventsDb);
   stmtMarkOldEvents->build("update %s set ", obj->eventsDb->TableName());
   stmtMarkOldEvents->build("updflg = case when updflg in (%s) then 'D' else updflg end, ", cEventState::getDeletable());
   stmtMarkOldEvents->build("delflg = 'Y', updsp = unix_timestamp()");
   stmtMarkOldEvents->build(" where source = '%s'", getSource());
   stmtMarkOldEvents->build(" and (source, fileref) not in (select source, fileref from fileref)");
   status += stmtMarkOldEvents->prepare();

   return status;
}

int Sngtv::exitDb()
{
   delete selectDistBySource;  selectDistBySource = nullptr;
   delete selectId;            selectId = nullptr;
   delete selectByName;        selectByName = nullptr;
   delete stmtMarkOldEvents;   stmtMarkOldEvents = nullptr;

   return success;
}

int Sngtv::ready()
{
   static int count {na};

   if (count == na)
   {
      char* where {};
      asprintf(&where, "source = '%s'", getSource());

      if (obj->mapDb->countWhere(where, count) != success)
         count = na;

      free(where);
   }

   return count > 0;
}

int Sngtv::cleanupAfter()
{
   stmtMarkOldEvents->execute();
   return success;
}

//***************************************************************************
// HTTP
//***************************************************************************

size_t Sngtv::collect(void* ptr, size_t size, size_t nmemb, void* user)
{
   ((std::string*)user)->append((const char*)ptr, size * nmemb);
   return size * nmemb;
}

int Sngtv::apiPost(const char* path, const char* body, std::string& response)
{
   std::string url = std::string(apiBase) + path;
   struct curl_slist* headers {};
   long httpCode {0};

   if (!handle)
   {
      if (!(handle = curl_easy_init()))
         return fail;

      curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");     // enable in-memory cookie engine
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(handle, CURLOPT_DNS_CACHE_TIMEOUT, 3600L);  // one lookup per hour is enough
      curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(handle, CURLOPT_USERAGENT, "epgd-sngtv/" SNGTV_VERSION);

      if (!isEmpty(EpgdConfig.proxy))
      {
         curl_easy_setopt(handle, CURLOPT_PROXY, EpgdConfig.proxy);

         if (!isEmpty(EpgdConfig.proxyuser))
         {
            curl_easy_setopt(handle, CURLOPT_PROXYUSERNAME, EpgdConfig.proxyuser);
            curl_easy_setopt(handle, CURLOPT_PROXYPASSWORD, EpgdConfig.proxypwd);
         }
      }
   }

   headers = curl_slist_append(headers, "Content-Type: application/json");
   headers = curl_slist_append(headers, "Accept: application/json");

   if (!csrfToken.empty())
      headers = curl_slist_append(headers, ("X_CSRFTOKEN: " + csrfToken).c_str());

   response.clear();

   curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
   curl_easy_setopt(handle, CURLOPT_POST, 1L);
   curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body);
   curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
   curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
   curl_easy_setopt(handle, CURLOPT_TIMEOUT, (long)timeout);

   CURLcode res = curl_easy_perform(handle);
   curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpCode);
   curl_slist_free_all(headers);

   if (requestDelay)
      usleep(requestDelay * 1000);

   if (res != CURLE_OK)
   {
      tell(eloError, "sngtv: Request '%s' failed, %s", path, curl_easy_strerror(res));
      return fail;
   }

   if (httpCode != 200)
   {
      tell(eloError, "sngtv: Request '%s' failed, http status %ld", path, httpCode);
      return fail;
   }

   return success;
}

//***************************************************************************
// Login (anonymous guest session)
//***************************************************************************

int Sngtv::login()
{
   std::string response;
   json_error_t error;

   csrfToken.clear();

   const char* body = "{\"terminalid\":\"00:00:00:00:00:00\",\"mac\":\"00:00:00:00:00:00\","
      "\"terminaltype\":\"WEBTV\",\"utcEnable\":1,\"timezone\":\"Etc/GMT0\","
      "\"userType\":3,\"terminalvendor\":\"Unknown\"}";

   if (apiPost("/Authenticate?SID=firstup&T=Windows_chrome_118", body, response) != success)
      return fail;

   json_t* root = json_loads(response.c_str(), 0, &error);

   if (const char* token = root ? jStr(root, "csrfToken") : nullptr)
      csrfToken = token;

   if (csrfToken.empty())
      tell(eloError, "sngtv: Login failed, '%s'", root && jStr(root, "retmsg") ? jStr(root, "retmsg") : "no token");
   else
      tell(eloEpgPlgDetail, "sngtv: Login succeeded");

   json_decref(root);

   return csrfToken.empty() ? fail : success;
}

//***************************************************************************
// Download one day of one channel
//***************************************************************************

int Sngtv::downloadDay(const char* extid, int day, std::string& json)
{
   std::string body = "{\"count\":-1,\"isFillProgram\":1,\"offset\":0,\"type\":2,"
      "\"properties\":[{\"name\":\"playbill\",\"include\":\"id,name,starttime,endtime,channelid,introduce,"
      "subName,seasonNum,subNum,genres,cast,country,producedate,externalIds,pictures\"}],"
      "\"begintime\":\"" + apiTime(day) + "\",\"endtime\":\"" + apiTime(day, 1) + "\","
      "\"channelid\":\"" + extid + "\"}";

   for (int retry = 0; retry < 2; retry++)
   {
      if (csrfToken.empty() && login() != success)
         return fail;

      if (apiPost("/PlayBillList", body.c_str(), json) == success && json.find("\"playbilllist\"") != std::string::npos)
         return success;

      // session expired or something - login again and retry once

      tell(eloEpgPlgDetail, "sngtv: Unexpected answer for '%s', retrying with new session", extid);
      csrfToken.clear();
   }

   return fail;
}

//***************************************************************************
// Process Day
//***************************************************************************

int Sngtv::processDay(int day, int fullupdate, Statistic* stat)
{
   obj->connection->startTransaction();

   obj->mapDb->clear();
   obj->mapDb->setValue("Source", getSource());

   for (int res = selectDistBySource->find(); res && !obj->doShutDown(); res = selectDistBySource->fetch())
   {
      std::string extid = obj->mapDb->getStrValue("ExternalId");
      std::string date = localDate(day);
      std::string name = extid + "-" + date;
      std::string json, xml;
      md5Buf md5 {};
      int count {0};

      // known file and its hash

      obj->fileDb->clear();
      obj->fileDb->setValue("Name", name.c_str());
      int known = selectByName->find();
      std::string lastTag = known ? obj->fileDb->getStrValue("Tag") : "";
      selectByName->freeResult();

      if (downloadDay(extid.c_str(), day, json) != success)
      {
         tell(eloInfo, "sngtv: Download of '%s' for %s failed", extid.c_str(), date.c_str());
         obj->fileDb->reset();
         continue;
      }

      // the picture option is part of the hash - toggling it forces a reload

      createMd5((json + (images ? "|images" : "")).c_str(), md5);

      if (!fullupdate && known && lastTag == md5)
      {
         tell(eloEpgPlgDetail, "sngtv: '%s' for %s not changed, skipping", extid.c_str(), date.c_str());
         stat->nonUpdates++;
         obj->fileDb->reset();
         continue;
      }

      stat->bytes += json.length();
      stat->files++;

      if (jsonToXml(json.c_str(), xml, count) != success)
      {
         stat->rejected++;
         obj->fileDb->reset();
         continue;
      }

      if (saveJson) saveFile(json, name + ".json");
      if (saveXml)  saveFile(xml, name + ".xml");

      std::string fileRef = name + "-" + md5;

      if (processXml(xml, extid.c_str(), fileRef.c_str()) != success)
      {
         stat->rejected++;
         obj->fileDb->reset();
         continue;
      }

      obj->fileDb->clear();
      obj->fileDb->setValue("Name", name.c_str());
      obj->fileDb->setValue("Source", getSource());
      obj->fileDb->setValue("ExternalId", extid.c_str());
      obj->fileDb->setValue("FileRef", fileRef.c_str());
      obj->fileDb->setValue("Tag", md5);

      if (known)
         obj->fileDb->update();
      else
         obj->fileDb->store();

      obj->fileDb->reset();

      obj->connection->commit();
      obj->connection->startTransaction();

      tell(eloEpgPlgDetail, "sngtv: '%s' for %s processed, %d events", extid.c_str(), date.c_str(), count);
   }

   obj->connection->commit();

   if (images && !obj->doShutDown())
      downloadImages();

   return success;
}

//***************************************************************************
// JSON -> intermediate XML (transformed by sngtv-*.xsl afterwards)
//***************************************************************************

int Sngtv::jsonToXml(const char* json, std::string& xml, int& count)
{
   json_error_t error;
   json_t* root = json_loads(json, 0, &error);
   json_t* list = root ? json_object_get(root, "playbilllist") : nullptr;

   count = 0;

   if (!json_is_array(list))
   {
      tell(eloInfo, "sngtv: Invalid json, %s", root ? "missing playbilllist" : error.text);
      json_decref(root);
      return fail;
   }

   xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sngtv>\n";

   size_t i;
   json_t* item;

   json_array_foreach(list, i, item)
   {
      const char* id = jStr(item, "id");
      time_t start = parseUtc(jStr(item, "starttime"));
      time_t end = parseUtc(jStr(item, "endtime"));

      if (isEmpty(id) || !atoll(id) || !start || end <= start)
         continue;

      xml += "<event id=\"" + std::to_string(eventIdOffset + atoll(id)) + "\">\n";
      xml += "  <starttime>" + std::to_string(start) + "</starttime>\n";
      xml += "  <duration>" + std::to_string(end - start) + "</duration>\n";

      addElement(xml, "title", jStr(item, "name"));
      addElement(xml, "subtitle", jStr(item, "subName"));
      addElement(xml, "description", jStr(item, "introduce"));
      addElement(xml, "genres", jStr(item, "genres"));
      addElement(xml, "season", jStr(item, "seasonNum"));
      addElement(xml, "episode", jStr(item, "subNum"));

      if (const char* c = jStr(item, "country"))
      {
         std::string country = c;
         for (auto& ch : country) ch = toupper(ch);
         addElement(xml, "country", country.c_str());
      }

      if (const char* d = jStr(item, "producedate"))
         addElement(xml, "year", std::string(d).substr(0, 4).c_str());

      if (json_t* cast = json_object_get(item, "cast"))
      {
         addElement(xml, "actor", jStr(cast, "actor"));
         addElement(xml, "director", jStr(cast, "director"));
         addElement(xml, "producer", jStr(cast, "producer"));
      }

      // external ids are a json string inside the json ...

      if (const char* ext = jStr(item, "externalIds"))
      {
         json_t* ids = json_loads(ext, 0, &error);
         size_t n;
         json_t* x;

         json_array_foreach(ids, n, x)
         {
            const char* type = jStr(x, "type");

            if (type && strcmp(type, "imdb") == 0)
               addElement(xml, "imdb", jStr(x, "id"));
         }

         json_decref(ids);
      }

      // pictures only if explicitly enabled (see README)

      if (images && EpgdConfig.getepgimages)
      {
         json_t* pics = json_object_get(item, "pictures");
         json_t* pick {};
         size_t n;
         json_t* p;

         json_array_foreach(pics, n, p)
         {
            const char* type = jStr(p, "imageType");

            if (!pick || (type && atoi(type) == imageType))
               pick = p;
         }

         if (const char* href = pick ? jStr(pick, "href") : nullptr)
         {
            std::string url = href;
            std::string file = url.substr(url.find_last_of('/') + 1);
            std::string::size_type dot = file.rfind(".jpg");

            if (dot != std::string::npos && dot == file.length() - 4)
            {
               addElement(xml, "image", file.substr(0, dot).c_str());
               pendingImages[file] = url;
            }
         }
      }

      xml += "</event>\n";
      count++;
   }

   xml += "</sngtv>\n";
   json_decref(root);

   return success;
}

//***************************************************************************
// Process (transformed) XML - store events
//***************************************************************************

int Sngtv::processXml(const std::string& xml, const char* extid, const char* fileRef)
{
   xmlDocPtr transformedDoc = obj->transformXml(xml.c_str(), xml.length(), pxsltStylesheet, fileRef);
   xmlNodePtr xmlRoot {};
   int count {0};

   if (!transformedDoc)
      return fail;

   if (!(xmlRoot = xmlDocGetRootElement(transformedDoc)))
   {
      tell(eloInfo, "sngtv: Invalid xml document returned from xslt for '%s', ignoring", fileRef);
      xmlFreeDoc(transformedDoc);
      return fail;
   }

   obj->mapDb->clear();
   obj->mapDb->setValue("ExternalId", extid);
   obj->mapDb->setValue("Source", getSource());

   for (int f = selectId->find(); f && obj->dbConnected(); f = selectId->fetch())
   {
      const char* channelId = obj->mapDb->getStrValue("ChannelId");
      time_t mergesp = obj->mapDb->getIntValue("MergeSp");
      long merge = obj->mapDb->getIntValue("Merge");

      for (xmlNodePtr node = xmlRoot->xmlChildrenNode; node && obj->dbConnected(); node = node->next)
      {
         char* prop {};
         tEventId eventid {0};

         if (node->type != XML_ELEMENT_NODE || strcmp((char*)node->name, "event") != 0)
            continue;

         if ((prop = (char*)xmlGetProp(node, (xmlChar*)"id")))
            eventid = atoll(prop);

         xmlFree(prop);

         if (!eventid)
            continue;

         obj->eventsDb->clear();
         obj->eventsDb->setBigintValue("EventId", eventid);
         obj->eventsDb->setValue("ChannelId", channelId);

         int insert = !obj->eventsDb->find();

         obj->eventsDb->setValue("Source", getSource());
         obj->eventsDb->setValue("FileRef", fileRef);

         obj->parseEvent(obj->eventsDb->getRow(), node);

         long starttime = obj->eventsDb->getIntValue("StartTime");
         char state = starttime <= mergesp ? cEventState::usInactive
            : merge > 1 ? cEventState::usMergeSpare : cEventState::usActive;

         if (insert)
         {
            obj->eventsDb->setValue("Version", 0xFF);
            obj->eventsDb->setValue("TableId", 0L);
            obj->eventsDb->setValue("UseId", 0L);
            obj->eventsDb->setCharValue("UpdFlg", state);
            obj->eventsDb->insert();
         }
         else
         {
            if (obj->eventsDb->hasValue("DelFlg", "Y"))
               obj->eventsDb->setValue("DelFlg", "N");

            if (obj->eventsDb->hasValue("UpdFlg", "D"))
               obj->eventsDb->setCharValue("UpdFlg", state);

            obj->eventsDb->update();
         }

         obj->eventsDb->reset();
         count++;
      }
   }

   selectId->freeResult();
   xmlFreeDoc(transformedDoc);

   tell(eloEpgPlgDetail, "sngtv: '%s' stored, %d events", fileRef, count);

   return success;
}

//***************************************************************************
// Pictures
//***************************************************************************

void Sngtv::downloadImages()
{
   int n {0};
   std::string path = std::string(EpgdConfig.cachePath) + "/" + getSource() + "/";

   for (const auto& [file, url] : pendingImages)
   {
      if (obj->doShutDown())
         break;

      if (fileExists((path + file).c_str()))
         continue;

      MemoryStruct data;
      int size {0};

      if (obj->downloadFile(url.c_str(), size, &data, timeout) != success)
      {
         tell(eloInfo, "sngtv: Download of picture '%s' failed", url.c_str());
         continue;
      }

      obj->storeToFs(&data, file.c_str(), getSource());
      n++;

      if (requestDelay)
         usleep(requestDelay * 1000);
   }

   pendingImages.clear();
   tell(eloEpgPlgDetail, "sngtv: Downloaded %d pictures", n);
}

int Sngtv::getPicture(const char* imagename, const char* fileRef, MemoryStruct* data)
{
   data->clear();

   if (!images)
      return 0;

   std::string file = std::string(EpgdConfig.cachePath) + "/" + getSource() + "/" + imagename;

   // not (yet) downloaded, e.g. due to a restart of epgd in between -> load it now,
   //   the url can be derived from the name: p<d1><d2>... -> .../gracenote/<d1>/<d2>/<name>

   if (!fileExists(file.c_str()) && imagename[0] == 'p' && isdigit(imagename[1]) && isdigit(imagename[2]))
   {
      int size {0};
      std::string url = std::string("http://ngiss.t-online.de/cm1s/media/gracenote/")
         + imagename[1] + "/" + imagename[2] + "/" + imagename;

      if (obj->downloadFile(url.c_str(), size, data, timeout) == success)
         return scalePicture(imagename, data);

      data->clear();
      tell(eloInfo, "sngtv: Download of picture '%s' failed", url.c_str());
      return 0;
   }

   obj->loadFromFs(data, imagename, getSource());

   // epgd keeps the picture in its database, no need to keep the file

   removeFile(file.c_str());

   return scalePicture(imagename, data);
}

//***************************************************************************
// Scale Picture
//   the provider offers the pictures only in their original resolution
//   (up to 1920x1080) - scale them down while decoding (libjpeg scales
//   in steps of 1/8) and re-encode them before epgd stores them
//***************************************************************************

int Sngtv::scalePicture(const char* imagename, MemoryStruct* data)
{
   std::vector<unsigned char> out;
   std::string error;
   unsigned w {0}, h {0};

   if (!scaleJpeg((const unsigned char*)data->memory, data->size, imageMaxWidth, imageMaxHeight,
                  imageQuality, out, w, h, error))
   {
      if (!error.empty())
         tell(eloInfo, "sngtv: Scaling of picture '%s' failed (%s), keeping original", imagename, error.c_str());

      return data->size;
   }

   tell(eloDebug, "sngtv: Scaled '%s' to %ux%u, %ld -> %zu bytes", imagename, w, h, (long)data->size, out.size());

   data->clear();
   data->memory = (char*)malloc(out.size());
   memcpy(data->memory, out.data(), out.size());
   data->size = out.size();

   return data->size;
}

void Sngtv::saveFile(const std::string& data, const std::string& name)
{
   std::string path = std::string(EpgdConfig.cachePath) + "/" + getSource() + "/";

   if (chkDir(path.c_str()) != success)
      return;

   FILE* fp = fopen((path + name).c_str(), "w");

   if (fp)
   {
      fwrite(data.c_str(), 1, data.length(), fp);
      fclose(fp);
   }
}

//***************************************************************************

extern "C" void* EPGPluginCreator() { return new Sngtv(); }
