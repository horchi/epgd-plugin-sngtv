/*
 * sngtv.h
 *
 * epgd plugin for the sngtv EPG JSON API
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#pragma once

#include <map>
#include <string>

#include <curl/curl.h>
#include <jansson.h>

#include "epgd.h"

#define SNGTV_VERSION "0.0.1"

//***************************************************************************
// Sngtv
//***************************************************************************

class Sngtv : public Plugin
{
   public:

      Sngtv();
      virtual ~Sngtv();

      int init(cEpgd* aObject, int aUtf8) override;
      int initDb() override;
      int exitDb() override;

      int atConfigItem(const char* Name, const char* Value) override;

      const char* getSource() override { return "sngtv"; }

      int getPicture(const char* imagename, const char* fileRef, MemoryStruct* data) override;
      int processDay(int day, int fullupdate, Statistic* stat) override;
      int cleanupAfter() override;
      int ready() override;

   private:

      // offset added to the provider ids to keep them apart from the ids of other sources
      //  (tvm uses its SendungID, currently ~3.3e8) while staying below 2^32

      static constexpr uint64_t eventIdOffset {3000000000ULL};

      int login();
      int apiPost(const char* path, const char* body, std::string& response);
      int downloadDay(const char* extid, int day, std::string& json);
      int jsonToXml(const char* json, std::string& xml, int& count);
      int processXml(const std::string& xml, const char* extid, const char* fileRef);
      void downloadImages();
      void saveFile(const std::string& data, const std::string& name);
      int scalePicture(const char* imagename, MemoryStruct* data);

      static size_t collect(void* ptr, size_t size, size_t nmemb, void* user);

      // config

      int images {no};               // download pictures, default OFF - see README!
      int imageType {17};            // preferred picture type (1: 4:3 1440x1080, 17: 16:9 1920x1080, 18: background)
      int imageMaxWidth {960};       // scale pictures down to fit into this box, 0 = no limit
      int imageMaxHeight {540};
      int imageQuality {80};         // jpeg quality of scaled pictures
      int requestDelay {250};        // [ms] pause between two API requests
      int timeout {30};              // [s]
      int saveJson {no};
      int saveXml {no};

      // state

      static xsltStylesheetPtr pxsltStylesheet;

      CURL* handle {};
      std::string csrfToken;
      std::map<std::string,std::string> pendingImages;   // file name -> url

      cDbStatement* selectDistBySource {};
      cDbStatement* selectId {};
      cDbStatement* selectByName {};
      cDbStatement* stmtMarkOldEvents {};
};
