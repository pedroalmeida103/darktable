/*
    This file is part of darktable,
    copyright (c) 2012 Pierre Lamot
    copyright (c) 2013 Jose Carlos Garcia Sogo

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dtgtk/button.h"
#include "dtgtk/label.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "common/pwstorage/pwstorage.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <libxml/parser.h>

DT_MODULE(2)

#define GOOGLE_WS_BASE_URL "https://accounts.google.com/"
#define GOOGLE_API_BASE_URL "https://www.googleapis.com/"
#define GOOGLE_API_KEY "428088086479.apps.googleusercontent.com"
#define GOOGLE_API_SECRET "tIIL4FUs46Nc9nQWKeg3H_Hy"
#define GOOGLE_URI "urn:ietf:wg:oauth:2.0:oob"
#define GOOGLE_PICASA "https://picasaweb.google.com/"

#define GOOGLE_IMAGE_MAX_SIZE 960

#define MSGCOLOR_RED "#e07f7f"
#define MSGCOLOR_GREEN "#7fe07f"

/** Authenticate against google picasa service*/
typedef struct _buffer_t
{
  char *data;
  size_t size;
  size_t offset;
} _buffer_t;

typedef enum ComboUserModel
{
  COMBO_USER_MODEL_NAME_COL = 0,
  COMBO_USER_MODEL_TOKEN_COL,
  COMBO_USER_MODEL_REFRESH_TOKEN_COL,
  COMBO_USER_MODEL_ID_COL,
  COMBO_USER_MODEL_NB_COL
} ComboUserModel;

typedef enum ComboAlbumModel
{
  COMBO_ALBUM_MODEL_NAME_COL = 0,
  COMBO_ALBUM_MODEL_ID_COL,
  COMBO_ALBUM_MODEL_NB_COL
} ComboAlbumModel;

typedef enum ComboPrivacyModel
{
  COMBO_PRIVACY_MODEL_NAME_COL = 0,
  COMBO_PRIVACY_MODEL_VAL_COL,
  COMBO_PRIVACY_MODEL_NB_COL
} ComboPrivacyModel;


/*
 * note: we don't support some kinds of privacy setting:
 *  CUSTOM : no plan to do this one for the moment
 *  NETWORKS_FRIENDS : this seems to be deprecated, it is currently impossible
 *      to create a new network (https://www.facebook.com/help/networks)
 */
typedef enum PicasaAlbumPrivacyPolicy
{
  PICASA_ALBUM_PRIVACY_PUBLIC,
  PICASA_ALBUM_PRIVACY_PRIVATE,
} PicasaAlbumPrivacyPolicy;


/**
 * Represents informations about an album
 */
typedef struct PicasaAlbum {
  gchar *id;
  gchar *name;
  PicasaAlbumPrivacyPolicy privacy;
} PicasaAlbum;

PicasaAlbum *picasa_album_init()
{
  return  (PicasaAlbum*) g_malloc0(sizeof(PicasaAlbum));
}

void picasa_album_destroy(PicasaAlbum *album)
{
  if (album == NULL)
    return;
  g_free(album->id);
  g_free(album->name);
  g_free(album);
}

/**
 * Represents informations about an account
 */
typedef struct FBAccountInfo {
  gchar *id;
  gchar *username;
  gchar *token;
  gchar *refresh_token;
} FBAccountInfo;

FBAccountInfo *picasa_account_info_init()
{
  return (FBAccountInfo*) g_malloc0(sizeof(FBAccountInfo));
}

void picasa_account_info_destroy(FBAccountInfo *account)
{
  if (account == NULL)
    return;
  g_free(account->id);
  g_free(account->username);
  g_free(account);
}

typedef struct PicasaContext
{
  gchar album_id[1024];
  gchar userid[1024];
  
  gchar *album_title;
  gchar *album_summary;
  int album_permission;
  
  /// curl context
  CURL *curl_ctx;
  /// Json parser context
  JsonParser *json_parser;

  GString *errmsg;

  /// authorization token
  gchar *token;
  gchar *refresh_token;
} PicasaContext;

typedef struct dt_storage_picasa_gui_data_t
{
  // == ui elements ==
  GtkLabel *label_username;
  GtkLabel *label_album;
  GtkLabel *label_status;

  GtkComboBox *comboBox_username;
  GtkButton *button_login;

  GtkDarktableButton *dtbutton_refresh_album;
  GtkComboBox *comboBox_album;

  //  === album creation section ===
  GtkLabel *label_album_title;
  GtkLabel *label_album_summary;
  GtkLabel *label_album_privacy;

  GtkEntry *entry_album_title;
  GtkEntry *entry_album_summary;
  GtkComboBox *comboBox_privacy;

  GtkBox *hbox_album;

  // == context ==
  gboolean connected;
  PicasaContext *picasa_api;
} dt_storage_picasa_gui_data_t;


static PicasaContext *picasa_api_init()
{
  PicasaContext *ctx = (PicasaContext*)g_malloc0(sizeof(PicasaContext));
  ctx->curl_ctx = curl_easy_init();
  ctx->errmsg = g_string_new("");
  ctx->json_parser = json_parser_new();
  return ctx;
}
#if 0
static void picasa_api_clean(PicasaContext *ctx)
{
  if (ctx == NULL)
    return;
  curl_easy_cleanup(ctx->curl_ctx);
  g_object_unref(ctx->json_parser);
  g_string_free(ctx->errmsg, TRUE);
}
#endif

static void picasa_api_destroy(PicasaContext *ctx)
{
  if (ctx == NULL)
    return;
  //FIXME: This is causing a segfault. Probably trying to dereference twice something already freed.
  //curl_easy_cleanup(ctx->curl_ctx);
  g_free(ctx->token);
  g_free(ctx->refresh_token);
  g_free(ctx->album_title);
  g_free(ctx->album_summary);
  g_object_unref(ctx->json_parser);
  g_string_free(ctx->errmsg, TRUE);
  g_free(ctx);
}


typedef struct dt_storage_picasa_param_t
{
  gint64 hash;
  PicasaContext *picasa_ctx;
} dt_storage_picasa_param_t;


static gchar *picasa_get_user_refresh_token(PicasaContext *ctx);

//////////////////////////// curl requests related functions

#if 0
/**
 * extract the user token from the calback @a url
 */
static gchar *picasa_extract_token_from_url(const gchar *url)
{
  g_return_val_if_fail((url != NULL), NULL);
  if(!(g_str_has_prefix(url, GOOGLE_WS_BASE_URL"connect/login_success.html") == TRUE)) return NULL;

  char *authtoken = NULL;

  char* *urlchunks = g_strsplit_set(url, "#&=", -1);
  //starts at 1 to skip the url prefix, then values are in the form key=value
  for (int i = 1; urlchunks[i] != NULL; i += 2)
  {
    if ((g_strcmp0(urlchunks[i], "access_token") == 0) && (urlchunks[i + 1] != NULL))
    {
      authtoken = g_strdup(urlchunks[i + 1]);
      break;
    }
    else if (g_strcmp0(urlchunks[i], "error") == 0)
    {
      break;
    }
    if (urlchunks[i + 1] == NULL) //this shouldn't happens but we never know...
    {
      g_printerr(_("[facebook] unexpeted url format\n"));
      break;
    }
  }
  g_strfreev(urlchunks);
  return authtoken;
}
#endif

/** Grow and fill _buffer_t with recieved data... */
static size_t _picasa_api_buffer_write_func(void *ptr, size_t size, size_t nmemb, void *stream)
{
  _buffer_t *buffer=(_buffer_t *)stream;
  char *newdata=g_malloc(buffer->size+nmemb+1);
  memset(newdata,0, buffer->size+nmemb+1);
  if( buffer->data != NULL ) memcpy(newdata, buffer->data, buffer->size);
  memcpy(newdata+buffer->size, ptr, nmemb);
  g_free( buffer->data );
  buffer->data = newdata;
  buffer->size += nmemb;
  return nmemb;
}

static size_t _picasa_api_buffer_read_func( void *ptr, size_t size, size_t nmemb, void *stream)
{
  _buffer_t *buffer=(_buffer_t *)stream;
  size_t dsize=0;
  if( (buffer->size - buffer->offset) > nmemb )
    dsize=nmemb;
  else
    dsize=(buffer->size - buffer->offset);

  memcpy(ptr,buffer->data+buffer->offset,dsize);
  buffer->offset+=dsize;
  return dsize;
}

static size_t curl_write_data_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
  GString *string = (GString*) data;
  g_string_append_len(string, ptr, size * nmemb);
#ifdef picasa_EXTRA_VERBOSE
  g_printf("server reply: %s\n", string->str);
#endif
  return size * nmemb;
}

static JsonObject *picasa_parse_response(PicasaContext *ctx, GString *response)
{
  GError *error = NULL;
  gboolean ret = json_parser_load_from_data(ctx->json_parser, response->str, response->len, &error);
  g_return_val_if_fail((ret), NULL);

  JsonNode *root = json_parser_get_root(ctx->json_parser);
  //we should always have a dict
  g_return_val_if_fail((json_node_get_node_type(root) == JSON_NODE_OBJECT), NULL);

  JsonObject *rootdict = json_node_get_object(root);
  if (json_object_has_member(rootdict, "error"))
  {
    JsonObject *errorstruct = json_object_get_object_member(rootdict, "error");
    g_return_val_if_fail((errorstruct != NULL), NULL);
    const gchar *errormessage = json_object_get_string_member(errorstruct, "message");
    g_return_val_if_fail((errormessage != NULL), NULL);
    g_string_assign(ctx->errmsg, errormessage);
    return NULL;
  }

  return rootdict;
}


static void picasa_query_get_add_url_arguments(GString *key, GString *value, GString *url)
{
  g_string_append(url, "&");
  g_string_append(url, key->str);
  g_string_append(url, "=");
  g_string_append(url, value->str);
}

/**
 * perform a GET request on facebook graph api
 *
 * @note use this one to read information (user info, existing albums, ...)
 *
 * @param ctx facebook context (token field must be set)
 * @param method the method to call on the facebook graph API, the methods should not start with '/' example: "me/albums"
 * @param args hashtable of the aguments to be added to the requests, must be in the form key (string) = value (string)
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *picasa_query_get(PicasaContext *ctx, const gchar *method, GHashTable *args, gboolean picasa)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);
  //build the query
  GString *url;
  if (picasa == TRUE)
    url = g_string_new(GOOGLE_PICASA);
  else
    url = g_string_new(GOOGLE_API_BASE_URL);
  
  g_string_append(url, method);

  if (picasa == TRUE)
  {
    g_string_append(url, "?alt=json&access_token=");
    g_string_append(url, ctx->token);
  }
  else
  {
    g_string_append(url, "?alt=json&access_token=");
    g_string_append(url, ctx->token);
  }
  if (args != NULL)
    g_hash_table_foreach(args, (GHFunc)picasa_query_get_add_url_arguments, url);

  //send the request
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
//#ifdef picasa_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
//#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);
  int res = curl_easy_perform(ctx->curl_ctx);

  if (res != CURLE_OK) return NULL;

  //parse the response
  JsonObject *respobj = picasa_parse_response(ctx, response);

  g_string_free(response, TRUE);
  g_string_free(url, TRUE);
  return respobj;
}

typedef struct {
  struct curl_httppost *formpost;
  struct curl_httppost *lastptr;
} HttppostFormList;
#if 0
static void picasa_query_post_add_form_arguments(const gchar *key, const gchar *value, HttppostFormList *formlist)
{
  curl_formadd(&(formlist->formpost),
    &(formlist->lastptr),
    CURLFORM_COPYNAME, key,
    CURLFORM_COPYCONTENTS, value,
    CURLFORM_END);
}

static void picasa_query_post_add_file_arguments(const gchar *key, const gchar *value, HttppostFormList *formlist)
{
  curl_formadd(&(formlist->formpost),
    &(formlist->lastptr),
    CURLFORM_COPYNAME, key,
    CURLFORM_COPYCONTENTS, value,
    CURLFORM_END);

  curl_formadd(&(formlist->formpost),
    &(formlist->lastptr),
    CURLFORM_COPYNAME, key,
    CURLFORM_FILE, value,
    CURLFORM_END);
}

/**
 * perform a POST request on facebook graph api
 *
 * @note use this one to create object (album, photos, ...)
 *
 * @param ctx facebook context (token field must be set) //FIXME
 * @param method the method to call on the facebook graph API, the methods should not start with '/' example: "me/albums"
 * @param args hashtable of the aguments to be added to the requests, might be null if none
 * @param files hashtable of the files to be send with the requests, might be null if none
 * @returns NULL if the request fails, or a JsonObject of the reply
 */

static JsonObject *picasa_query_post(PicasaContext *ctx, const gchar *method, GHashTable *args, GHashTable *files, gboolean alt_url)
{
  g_return_val_if_fail(ctx != NULL, NULL);
//  g_return_val_if_fail(ctx->token != NULL, NULL);

  GString *url = NULL;

  if (alt_url == TRUE)
    url = g_string_new(GOOGLE_WS_BASE_URL);
  else
    url =g_string_new(GOOGLE_API_BASE_URL);
  g_string_append(url, method);

  HttppostFormList formlist;
  formlist.formpost = NULL;
  formlist.lastptr = NULL;

  if (ctx->token != NULL)
    curl_formadd(&(formlist.formpost),
      &(formlist.lastptr),
      CURLFORM_COPYNAME, "access_token",
      CURLFORM_COPYCONTENTS, ctx->token,
      CURLFORM_END);

  if (args != NULL)
    g_hash_table_foreach(args, (GHFunc)picasa_query_post_add_form_arguments, &formlist);

  if (files != NULL)
    g_hash_table_foreach(files, (GHFunc)picasa_query_post_add_file_arguments, &formlist);

  //send the requests
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
//#ifdef picasa_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
//#endif
  //curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPPOST, formlist.formpost);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);

  int res = curl_easy_perform(ctx->curl_ctx);
  curl_formfree(formlist.formpost);
  g_string_free(url, TRUE);
  if (res != CURLE_OK) return NULL;
  //parse the response
  JsonObject *respobj = picasa_parse_response(ctx, response);
  g_string_free(response, TRUE);
  return respobj;
}
#endif

static JsonObject *picasa_query_post_auth(PicasaContext *ctx, const gchar *method, gchar *args)
{
  g_return_val_if_fail(ctx != NULL, NULL);

  GString *url = NULL;

  url = g_string_new(GOOGLE_WS_BASE_URL);
  g_string_append(url, method);

  //send the requests
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_COPYPOSTFIELDS, args);
//#ifdef picasa_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
//#endif
  //curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPPOST, formlist.formpost);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);

  int res = curl_easy_perform(ctx->curl_ctx);
//  curl_formfree(formlist.formpost);
  g_string_free(url, TRUE);
  if (res != CURLE_OK) return NULL;
  //parse the response
  JsonObject *respobj = picasa_parse_response(ctx, response);
  g_string_free(response, TRUE);
  return respobj;
}

//////////////////////////// facebook api functions

/**
 * @returns TRUE if the current token is valid
 */
static gboolean picasa_test_auth_token(PicasaContext *ctx)
{
  //JsonObject *obj = picasa_query_get(ctx, "oauth2/v1/userinfo", NULL, FALSE);
  //return obj != NULL;
  gchar *access_token = NULL;
  access_token = picasa_get_user_refresh_token(ctx);
  
  if (access_token != NULL)
    ctx->token = access_token;

  return access_token != NULL;
}

/**
 * @return a GList of PicasaAlbums associated to the user
 */
static GList *picasa_get_album_list(PicasaContext *ctx, gboolean* ok)
{
  if (ok) *ok = TRUE;
  GList *album_list = NULL;

  JsonObject *reply = picasa_query_get(ctx, "data/feed/api/user/default", NULL, TRUE);
  if (reply == NULL)
    goto error;

  JsonObject *feed = json_object_get_object_member(reply, "feed");
  if (feed == NULL)
    goto error;

  //json_object_foreach_member (jsalbums, (JsonObjectForeach)get_album_info, album_list);
  JsonArray *jsalbums = json_object_get_array_member(feed, "entry");

  guint i;
  for (i = 0; i < json_array_get_length(jsalbums); i++)
  {
    JsonObject *obj = json_array_get_object_element(jsalbums, i);
    if (obj == NULL)
      continue;

  //  JsonNode* canupload_node = json_object_get_member(obj, "can_upload");
  //  if (canupload_node == NULL || !json_node_get_boolean(canupload_node))
  //    continue;

    PicasaAlbum *album = picasa_album_init();
    if (album == NULL)
      goto error;

    JsonObject *jsid = json_object_get_object_member(obj, "gphoto$id");
    JsonObject *jstitle = json_object_get_object_member(obj, "title");

    const char* id = json_object_get_string_member(jsid, "$t");
    const char* name = json_object_get_string_member(jstitle, "$t");
    if (id == NULL || name == NULL) {
      picasa_album_destroy(album);
      goto error;
    }
    album->id = g_strdup(id);
    album->name = g_strdup(name);
    album_list = g_list_append(album_list, album);
  }
  return album_list;

error:
  *ok = FALSE;
  g_list_free_full(album_list, (GDestroyNotify)picasa_album_destroy);
  return NULL;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/user/
 * @return the id of the newly reacted
 */
static const gchar *picasa_create_album(PicasaContext *ctx, gchar *name, gchar *summary, PicasaAlbumPrivacyPolicy privacy)
{
  _buffer_t buffer;
  memset(&buffer,0,sizeof(_buffer_t));
  
  gchar *photo_id=NULL;
  gchar *private = NULL;
  char uri[4096]= {0};
  struct curl_slist *headers = NULL;
  
  if ( privacy == PICASA_ALBUM_PRIVACY_PUBLIC)
    private = g_strdup ("public");
  else
    private = g_strdup ("private");

  gchar *entry = g_markup_printf_escaped (
                 "<entry xmlns='http://www.w3.org/2005/Atom'\n"
                      "xmlns:media='http://search.yahoo.com/mrss/'\n"
                      "xmlns:gphoto='http://schemas.google.com/photos/2007'>\n"
                    "<title type='text'>%s</title>\n"
                    "<summary type='text'>%s</summary>\n"
                    "<gphoto:access>%s</gphoto:access>\n"
                    "<media:group>\n"
                    "  <media:keywords></media:keywords>\n"
                    "</media:group>\n"
                    "<category scheme='http://schemas.google.com/g/2005#kind'\n"
                    "  term='http://schemas.google.com/photos/2007#album'></category>\n"
                    "</entry>\n", 
                    name, summary, private);

  gchar *authHeader = NULL;
  authHeader = dt_util_dstrcat(authHeader, "Authorization: OAuth %s", ctx->token);

  headers = curl_slist_append(headers,"GData-Version: 2");
  headers = curl_slist_append(headers,"Content-Type: application/atom+xml");
  headers = curl_slist_append(headers, authHeader);

  sprintf(uri,"https://picasaweb.google.com/data/feed/api/user/default");
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, uri);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST,1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POSTFIELDS, entry);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, &buffer);
  
  curl_easy_perform( ctx->curl_ctx );

  curl_slist_free_all(headers);

  long result;
  curl_easy_getinfo(ctx->curl_ctx, CURLINFO_RESPONSE_CODE, &result);

  printf("Buffer: %s\n", buffer.data);
  
  if (result == 201)
  {
    xmlDocPtr doc;
    xmlNodePtr entryNode;
    // Parse xml document
    if( ( doc = xmlParseDoc( (xmlChar *)buffer.data ))==NULL) return 0;
    entryNode = xmlDocGetRootElement(doc);
    if(  !xmlStrcmp(entryNode->name, (const xmlChar *) "entry") )
    {
      xmlNodePtr entryChilds = entryNode->xmlChildrenNode;
      if( entryChilds != NULL )
      {
        do
        {
          if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"id")) )
          {
            // Get the album id
            xmlChar *id= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            if( xmlStrncmp( id, (const xmlChar *)"http://",7) )
              photo_id = g_strdup((const char *)id);
            xmlFree(id);
          }
        }
        while( (entryChilds = entryChilds->next)!=NULL );
      }
    }

  return photo_id;
  }

  return NULL;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/album/
 * @return the id of the uploaded photo
 */
#if 0
static const gchar *picasa_upload_photo_to_album(PicasaContext *ctx, gchar *albumid, gchar *fpath, gchar *description)
{
  GString *method = g_string_new(albumid);
  g_string_append(method, "/photos");

  GHashTable *files = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
  g_hash_table_insert(files, "source", fpath);

  GHashTable *args = NULL;
  if (description != NULL)
  {
    args = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
    g_hash_table_insert(args, "message", description);
  }

  JsonObject *ref = picasa_query_post(ctx, method->str, args, files, FALSE);
  g_string_free(method, TRUE);

  g_hash_table_destroy(files);
  if (args != NULL)
  {
    g_hash_table_destroy(args);
  }
  if (ref == NULL)
    return NULL;
  return json_object_get_string_member(ref, "id");
}
#endif

static const gchar *picasa_upload_photo_to_album(PicasaContext *ctx, gchar *albumid, gchar *fname, gchar *caption, gchar *description, const int imgid)
{
  _buffer_t buffer;
  memset(&buffer,0,sizeof(_buffer_t));
  gchar *photo_id=NULL;
  
  char uri[4096]= {0};

  // Open the temp file and read image to memory
  GMappedFile *imgfile = g_mapped_file_new(fname,FALSE,NULL);
  int size = g_mapped_file_get_length( imgfile );
  gchar *data =g_mapped_file_get_contents( imgfile );


  gchar *entry = g_markup_printf_escaped (
                   "<entry xmlns='http://www.w3.org/2005/Atom'>\n"
                   "<title>%s</title>\n"
                   "<summary>%s</summary>\n"
                   "<category scheme=\"http://schemas.google.com/g/2005#kind\"\n"
                   " term=\"http://schemas.google.com/photos/2007#photo\"/>"
                   "</entry>",
                   caption, description);

  gchar *authHeader = NULL;
  authHeader = dt_util_dstrcat(authHeader, "Authorization: OAuth %s", ctx->token);

  // Hack for nonform multipart post...
  gchar mpart1[4096]= {0};
  gchar *mpart_format="\nMedia multipart posting\n--END_OF_PART\nContent-Type: application/atom+xml\n\n%s\n--END_OF_PART\nContent-Type: image/jpeg\n\n";
  sprintf(mpart1,mpart_format,entry);

  int mpart1size=strlen(mpart1);
  int postdata_length=mpart1size+size+strlen("\n--END_OF_PART--");
  gchar *postdata=g_malloc(postdata_length);
  memcpy( postdata, mpart1, mpart1size);
  memcpy( postdata+mpart1size, data, size);
  memcpy( postdata+mpart1size+size, "\n--END_OF_PART--",strlen("\n--END_OF_PART--") );

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers,"Content-Type: multipart/related; boundary=\"END_OF_PART\"");
  headers = curl_slist_append(headers,"MIME-version: 1.0");
  headers = curl_slist_append(headers,"Expect:");
  headers = curl_slist_append(headers,"GData-Version: 2");
  headers = curl_slist_append(headers, authHeader);

  sprintf(uri,"https://picasaweb.google.com/data/feed/api/user/default/albumid/%s", albumid);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, uri);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_UPLOAD,0);   // A post request !
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST,1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POSTFIELDS, postdata);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POSTFIELDSIZE, postdata_length);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, &buffer);

  curl_easy_perform( ctx->curl_ctx );

  curl_slist_free_all(headers);

  long result;
  curl_easy_getinfo(ctx->curl_ctx,CURLINFO_RESPONSE_CODE,&result );

  // If we want to add tags let's do...
  if( result == 201 && imgid > 0 )
  {
    // Image was created , fine.. and result have the fully created photo xml entry..
    // Let's perform an update of the photos keywords with tags passed along to this function..
    // and use picasa photo update api to add keywords to the photo...

    // Build the keywords content string
    gchar *keywords = NULL;
    keywords = dt_tag_get_list(imgid, ",");

    xmlDocPtr doc;
    xmlNodePtr entryNode;
    // Parse xml document
    if( ( doc = xmlParseDoc( (xmlChar *)buffer.data ))==NULL) return 0;
    entryNode = xmlDocGetRootElement(doc);
    if(  !xmlStrcmp(entryNode->name, (const xmlChar *) "entry") )
    {
      // Let's get the gd:etag attribute of entry...
      // For now, we force update using "If-Match: *"
      /*
        if( !xmlHasProp(entryNode, (const xmlChar*)"gd:etag") ) return 0;
        xmlChar *etag = xmlGetProp(entryNode,(const xmlChar*)"gd:etag");
      */

      gchar *updateUri=NULL;
      xmlNodePtr entryChilds = entryNode->xmlChildrenNode;
      if( entryChilds != NULL )
      {
        do
        {
          if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"id")) )
          {
            // Get the photo id used in uri for update
            xmlChar *id= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            if( xmlStrncmp( id, (const xmlChar *)"http://",7) )
              photo_id = g_strdup((const char *)id);
            xmlFree(id);
          }
          else  if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"group")) )
          {
            // Got the media:group entry lets find the child media:keywords
            xmlNodePtr mediaChilds = entryChilds->xmlChildrenNode;
            if(mediaChilds != NULL)
            {
              do
              {
                // Got the keywords tag, let's set the tags
                if ((!xmlStrcmp(mediaChilds->name, (const xmlChar *)"keywords")) )
                  xmlNodeSetContent(mediaChilds, (xmlChar *)keywords);
              }
              while( (mediaChilds = mediaChilds->next)!=NULL );
            }
          }
          else if (( !xmlStrcmp(entryChilds->name,(const xmlChar*)"link")) )
          {
            xmlChar *rel = xmlGetProp(entryChilds,(const xmlChar*)"rel");
            if( !xmlStrcmp(rel,(const xmlChar *)"edit") )
            {
              updateUri=(char *)xmlGetProp(entryChilds,(const xmlChar*)"href");
            }
            xmlFree(rel);
          }
        }
        while( (entryChilds = entryChilds->next)!=NULL );
      }

      // Let's update the photo
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers,"Content-Type: application/atom+xml");
      headers = curl_slist_append(headers,"If-Match: *");
      headers = curl_slist_append(headers,"Expect:");
      headers = curl_slist_append(headers,"GData-Version: 2");
      headers = curl_slist_append(headers, authHeader);

      _buffer_t response;
      memset(&response,0,sizeof(_buffer_t));

      // Setup data to send..
      _buffer_t writebuffer;
      int datasize;
      xmlDocDumpMemory(doc,(xmlChar **)&writebuffer.data, &datasize);
      writebuffer.size = datasize;
      writebuffer.offset=0;

     // updateUri = dt_util_dstrcat(updateUri, "?access_token=%s", ctx->token);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, updateUri);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_UPLOAD,1);   // A put request
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_READDATA,&writebuffer);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_INFILESIZE,writebuffer.size);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_READFUNCTION,_picasa_api_buffer_read_func);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, &response);
      curl_easy_perform( ctx->curl_ctx );

      xmlFree( updateUri );
      xmlFree( writebuffer.data );
      if (response.data != NULL)
        g_free(response.data);

      curl_slist_free_all( headers );
    }

    xmlFreeDoc(doc);
  }
  return photo_id;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/user/
 * @return basic informations about the account
 */
static FBAccountInfo *picasa_get_account_info(PicasaContext *ctx)
{
  JsonObject *obj = picasa_query_get(ctx, "oauth2/v1/userinfo", NULL, FALSE);
  g_return_val_if_fail((obj != NULL), NULL);
  const gchar *user_name = json_object_get_string_member(obj, "name");
  const gchar *user_id = json_object_get_string_member(obj, "id");
  g_return_val_if_fail(user_name != NULL && user_id != NULL, NULL);
  FBAccountInfo *accountinfo = picasa_account_info_init();
  accountinfo->id = g_strdup(user_id);
  accountinfo->username = g_strdup(user_name);
  accountinfo->token = g_strdup(ctx->token);
  accountinfo->refresh_token = g_strdup(ctx->refresh_token);

  g_snprintf(ctx->userid, 1024, "%s", user_id);
  return accountinfo;
}


///////////////////////////////// UI functions

static gboolean combobox_separator(GtkTreeModel *model,GtkTreeIter *iter,gpointer data)
{
  GValue value = { 0, };
  gtk_tree_model_get_value(model,iter,0,&value);
  gchar *v=NULL;
  if (G_VALUE_HOLDS_STRING (&value))
  {
    if( (v=(gchar *)g_value_get_string (&value))!=NULL && strlen(v) == 0 ) return TRUE;
  }
  return FALSE;
}

static gchar *picasa_get_user_refresh_token(PicasaContext *ctx)
{
  gchar *refresh_token = NULL;
  JsonObject *reply;
  gchar *params = NULL;
  
  params = dt_util_dstrcat(params, "refresh_token=%s&client_id="GOOGLE_API_KEY"&client_secret="GOOGLE_API_SECRET"&grant_type=refresh_token", ctx->refresh_token);
  printf ("URL: %s", params);
  
  reply = picasa_query_post_auth(ctx, "o/oauth2/token", params);
 
  refresh_token = g_strdup(json_object_get_string_member(reply, "access_token"));

  g_free (params);

  return refresh_token;
}

/**
 * @see https://developers.facebook.com/docs/authentication/
 * @returs NULL if the user cancel the operation or a valid token
 */
static int picasa_get_user_auth_token(dt_storage_picasa_gui_data_t *ui)
{
  ///////////// open the authentication url in a browser
  GError *error = NULL;
  gtk_show_uri(gdk_screen_get_default(),
               GOOGLE_WS_BASE_URL"o/oauth2/auth?"
               "client_id=" GOOGLE_API_KEY
               "&redirect_uri=urn:ietf:wg:oauth:2.0:oob"
               "&scope=https://picasaweb.google.com/data/ https://www.googleapis.com/auth/userinfo.profile"
               "&response_type=code", gtk_get_current_event_time(), &error);

  ////////////// build & show the validation dialog
  gchar *text1 = _("step 1: a new window or tab of your browser should have been "
                   "loaded. you have to login into your picasa account there "
                   "and authorize darktable to upload photos before continuing.");
  gchar *text2 = _("step 2: paste your browser url and click the ok button once "
                   "you are done.");

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *picasa_auth_dialog = GTK_DIALOG(gtk_message_dialog_new (GTK_WINDOW (window),
                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_MESSAGE_QUESTION,
                                  GTK_BUTTONS_OK_CANCEL,
                                  _("Picasa authentication")));
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (picasa_auth_dialog),
      "%s\n\n%s", text1, text2);

  GtkWidget *entry = gtk_entry_new();
  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(gtk_label_new(_("url:"))), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(entry), TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(picasa_auth_dialog->vbox), hbox, TRUE, TRUE, 0);

  gtk_widget_show_all(GTK_WIDGET(picasa_auth_dialog));

  ////////////// wait for the user to entrer the validation URL
  gint result;
  gchar *token = NULL;
  const char *replyurl;
  while (TRUE)
  {
    result = gtk_dialog_run (GTK_DIALOG (picasa_auth_dialog));
    if (result == GTK_RESPONSE_CANCEL)
      break;
    replyurl = gtk_entry_get_text(GTK_ENTRY(entry));
    if (replyurl == NULL || g_strcmp0(replyurl, "") == 0)
    {
      gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(picasa_auth_dialog),
                                                 "%s\n\n%s\n\n<span foreground=\"" MSGCOLOR_RED "\" ><small>%s</small></span>",
                                                 text1, text2, _("please enter the validation url"));
      continue;
    }
    //token = picasa_extract_token_from_url(replyurl);
    token = g_strdup(replyurl);
    if (token != NULL)//we have a valid token
      break;
    else
      gtk_message_dialog_format_secondary_markup(
            GTK_MESSAGE_DIALOG(picasa_auth_dialog),
            "%s\n\n%s%s\n\n<span foreground=\"" MSGCOLOR_RED "\"><small>%s</small></span>",
            text1, text2,
            _("the given url is not valid, it should look like: "),
              GOOGLE_WS_BASE_URL"connect/login_success.html?...");
  }
  gtk_widget_destroy(GTK_WIDGET(picasa_auth_dialog));

  // Interchange now the authorization_code for an access_token and refresh_token
  JsonObject *reply;

  gchar *params = NULL;
  params = dt_util_dstrcat(params, "code=%s&client_id="GOOGLE_API_KEY"&client_secret="GOOGLE_API_SECRET"&redirect_uri="GOOGLE_URI"&grant_type=authorization_code", token);
  printf ("URL: %s", params);
  
  reply = picasa_query_post_auth(ui->picasa_api, "o/oauth2/token", params);
 
  gchar *access_token = g_strdup(json_object_get_string_member(reply, "access_token"));

  gchar *refresh_token = g_strdup(json_object_get_string_member(reply, "refresh_token"));

  ui->picasa_api->token = access_token;
  ui->picasa_api->refresh_token = refresh_token;

  g_free (params);

  return 0; //FIXME
}


static void load_account_info_fill(gchar *key, gchar *value, GSList **accountlist)
{
  FBAccountInfo *info = picasa_account_info_init();
  info->id = g_strdup(key);
//FIXME
  JsonParser *parser = json_parser_new();
  json_parser_load_from_data(parser, value, strlen(value), NULL);
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = json_node_get_object(root);
  info->token = g_strdup(json_object_get_string_member(obj, "token"));
  info->username = g_strdup(json_object_get_string_member(obj, "username"));
  info->id = g_strdup(json_object_get_string_member(obj, "userid"));
  info->refresh_token = g_strdup(json_object_get_string_member(obj, "refresh_token"));
  *accountlist =  g_slist_prepend(*accountlist, info);
  g_object_unref(parser);
}

/**
 * @return a GSList of saved FBAccountInfo
 */
static GSList *load_account_info()
{
  GSList *accountlist = NULL;

  GHashTable *table = dt_pwstorage_get("picasa2");
  g_hash_table_foreach(table, (GHFunc) load_account_info_fill, &accountlist);
  return accountlist;
}

static void save_account_info(dt_storage_picasa_gui_data_t *ui, FBAccountInfo *accountinfo)
{
  PicasaContext *ctx = ui->picasa_api;
  g_return_if_fail(ctx != NULL);

  ///serialize data;
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "username");
  json_builder_add_string_value(builder, accountinfo->username);
  json_builder_set_member_name(builder, "userid");
  json_builder_add_string_value(builder, accountinfo->id);
  json_builder_set_member_name(builder, "token");
  json_builder_add_string_value(builder, accountinfo->token);
  json_builder_set_member_name(builder, "refresh_token");
  json_builder_add_string_value(builder, accountinfo->refresh_token);

  json_builder_end_object(builder);

  JsonNode *node = json_builder_get_root(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, node);
#if JSON_CHECK_VERSION(0, 14, 0)
  json_generator_set_pretty(generator, FALSE);
#endif
  gchar *data = json_generator_to_data(generator, NULL);

  json_node_free(node);
  g_object_unref(generator);
  g_object_unref(builder);

  GHashTable *table = dt_pwstorage_get("picasa2");
  g_hash_table_insert(table, accountinfo->id, data);
  dt_pwstorage_set("picasa2", table);

  g_hash_table_destroy(table);
}

static void remove_account_info(const gchar *accountid)
{
  GHashTable *table = dt_pwstorage_get("picasa2");
  g_hash_table_remove(table, accountid);
  dt_pwstorage_set("picasa2", table);
  g_hash_table_destroy(table);
}

static void ui_refresh_users_fill(FBAccountInfo *value, gpointer dataptr)
{
  GtkListStore *liststore = GTK_LIST_STORE(dataptr);
  GtkTreeIter iter;
  gtk_list_store_append(liststore, &iter);
  gtk_list_store_set(liststore, &iter,
                     COMBO_USER_MODEL_NAME_COL, value->username,
                     COMBO_USER_MODEL_TOKEN_COL, value->token,
                     COMBO_USER_MODEL_REFRESH_TOKEN_COL, value->refresh_token,
                     COMBO_USER_MODEL_ID_COL, value->id, -1);
}

static void ui_refresh_users(dt_storage_picasa_gui_data_t *ui)
{
  GSList *accountlist= load_account_info();
  GtkListStore *list_store = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
  GtkTreeIter iter;

  gtk_list_store_clear(list_store);
  gtk_list_store_append(list_store, &iter);

  if (g_slist_length(accountlist) == 0)
  {
    gtk_list_store_set(list_store, &iter,
                       COMBO_USER_MODEL_NAME_COL, _("new account"),
                       COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL, -1);
  }
  else
  {
    gtk_list_store_set(list_store, &iter,
                       COMBO_USER_MODEL_NAME_COL, _("other account"),
                       COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL,-1);
    gtk_list_store_append(list_store, &iter);
    gtk_list_store_set(list_store, &iter,
                       COMBO_USER_MODEL_NAME_COL, "",
                       COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL,-1);//separator
  }

  g_slist_foreach(accountlist, (GFunc)ui_refresh_users_fill, list_store);
  gtk_combo_box_set_active(ui->comboBox_username, 0);

  g_slist_free_full(accountlist, (GDestroyNotify)picasa_account_info_destroy);
  gtk_combo_box_set_row_separator_func(ui->comboBox_username,combobox_separator,ui->comboBox_username,NULL);
}

static void ui_refresh_albums_fill(PicasaAlbum *album, GtkListStore *list_store)
{
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_ALBUM_MODEL_NAME_COL, album->name, COMBO_ALBUM_MODEL_ID_COL, album->id, -1);
}

static void ui_refresh_albums(dt_storage_picasa_gui_data_t *ui)
{
  gboolean getlistok;
  GList *albumList = picasa_get_album_list(ui->picasa_api, &getlistok);
  if (! getlistok)
  {
    dt_control_log(_("unable to retreive the album list"));
    goto cleanup;
  }

  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  GtkTreeIter iter;
  gtk_list_store_clear(model_album);
  gtk_list_store_append(model_album, &iter);
  gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, _("create new album"), COMBO_ALBUM_MODEL_ID_COL, NULL, -1);
  if (albumList != NULL)
  {
    gtk_list_store_append(model_album, &iter);
    gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, "", COMBO_ALBUM_MODEL_ID_COL, NULL, -1); //separator
  }
  g_list_foreach(albumList, (GFunc)ui_refresh_albums_fill, model_album);

  if (albumList != NULL)
    gtk_combo_box_set_active(ui->comboBox_album, 2);
    // FIXME: get the albumid and set it in the PicasaCtx
  else
    gtk_combo_box_set_active(ui->comboBox_album, 0);

  gtk_widget_show_all(GTK_WIDGET(ui->comboBox_album));
  g_list_free_full(albumList, (GDestroyNotify)picasa_album_destroy);

cleanup:
  return;
}

static void ui_reset_albums_creation(struct dt_storage_picasa_gui_data_t *ui)
{
  //GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  //gtk_list_store_clear(model_album);
  gtk_entry_set_text(ui->entry_album_summary, "");
  gtk_entry_set_text(ui->entry_album_title, "");
  gtk_widget_hide_all(GTK_WIDGET(ui->hbox_album));
}

static void ui_combo_username_changed(GtkComboBox *combo, struct dt_storage_picasa_gui_data_t *ui)
{
  printf("Combo username changed called\n");
  GtkTreeIter iter;
  gchar *token = NULL;
  gchar *refresh_token = NULL;
  gchar *userid = NULL;
  if (!gtk_combo_box_get_active_iter(combo, &iter))
    return; //ie: list is empty while clearing the combo
  GtkTreeModel *model = gtk_combo_box_get_model(combo);
  gtk_tree_model_get( model, &iter, COMBO_USER_MODEL_TOKEN_COL, &token, -1);//get the selected token
  gtk_tree_model_get( model, &iter, COMBO_USER_MODEL_REFRESH_TOKEN_COL, &refresh_token, -1);
  gtk_tree_model_get( model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);

  ui->picasa_api->token = g_strdup(token);
  ui->picasa_api->refresh_token = g_strdup(refresh_token);
  g_snprintf(ui->picasa_api->userid, 1024, "%s", userid);

  if (ui->picasa_api->token != NULL && picasa_test_auth_token(ui->picasa_api))
  {
    ui->connected = TRUE;
    gtk_button_set_label(ui->button_login, _("logout"));
    ui_refresh_albums(ui);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), TRUE);
  }
  else
  {
    gtk_button_set_label(ui->button_login, _("login"));
    g_free(ui->picasa_api->token);
    g_free(ui->picasa_api->refresh_token);
    ui->picasa_api->token = ui->picasa_api->refresh_token = NULL;
    gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
    GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
    gtk_list_store_clear(model_album);
  }
  ui_reset_albums_creation(ui);
}

static void ui_combo_album_changed(GtkComboBox *combo, gpointer data)
{
  dt_storage_picasa_gui_data_t *ui = (dt_storage_picasa_gui_data_t*)data;

  GtkTreeIter iter;
  gchar *albumid = NULL;
  if (gtk_combo_box_get_active_iter(combo, &iter))
  {
    GtkTreeModel  *model = gtk_combo_box_get_model( combo );
    gtk_tree_model_get( model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);//get the album id
  }

  if (albumid == NULL)
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->hbox_album));
  }
  else
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), TRUE);
    gtk_widget_hide(GTK_WIDGET(ui->hbox_album));
  }

}


static gboolean ui_authenticate(dt_storage_picasa_gui_data_t *ui)
{
  if (ui->picasa_api == NULL)
  {
    ui->picasa_api = picasa_api_init();
  }

  PicasaContext *ctx = ui->picasa_api;
  gboolean mustsaveaccount = FALSE;

  gchar *uiselectedaccounttoken = NULL;
  gchar *uiselectedaccountrefreshtoken = NULL;
  gchar *uiselecteduserid = NULL;
  GtkTreeIter iter;
  gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
  GtkTreeModel *accountModel = gtk_combo_box_get_model(ui->comboBox_username);
  gtk_tree_model_get(accountModel, &iter, COMBO_USER_MODEL_TOKEN_COL, &uiselectedaccounttoken, -1);
  gtk_tree_model_get(accountModel, &iter, COMBO_USER_MODEL_REFRESH_TOKEN_COL, &uiselectedaccountrefreshtoken, -1);
  gtk_tree_model_get(accountModel, &iter, COMBO_USER_MODEL_ID_COL, &uiselecteduserid, -1);

  if (ctx->token != NULL)
  {
    g_free(ctx->token);
    g_free(ctx->refresh_token);
    ctx->userid[0] = 0;
    ctx->token = ctx->refresh_token = NULL;
  }

  if (uiselectedaccounttoken != NULL)
  {
    ctx->token = g_strdup(uiselectedaccounttoken);
    ctx->refresh_token = g_strdup(uiselectedaccountrefreshtoken);
    g_snprintf(ctx->userid, 1024, "%s", uiselecteduserid);
  }
  //check selected token if we already have one
  if (ctx->token != NULL && !picasa_test_auth_token(ctx))
  {
      g_free(ctx->token);
      g_free(ctx->refresh_token);
      ctx->userid[0] = 0;
      ctx->token = ctx->refresh_token = NULL;
  }

  int ret;
  if(ctx->token == NULL)
  {
    mustsaveaccount = TRUE;
    ret = picasa_get_user_auth_token(ui);//ask user to log in
  }

  if (ctx->token == NULL || ctx->refresh_token == NULL || ret != 0)
  {
    return FALSE;
  }
  else
  {
    if (mustsaveaccount)
    {
      // Get first the refresh token
      FBAccountInfo *accountinfo = picasa_get_account_info(ui->picasa_api);
      g_return_val_if_fail(accountinfo != NULL, FALSE);
      save_account_info(ui, accountinfo);

      //add account to user list and select it
      GtkListStore *model =  GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
      GtkTreeIter iter;
      gboolean r;
      gchar *uid;

      gboolean updated = FALSE;

      for (r = gtk_tree_model_get_iter_first (GTK_TREE_MODEL(model), &iter);
           r == TRUE;
           r = gtk_tree_model_iter_next (GTK_TREE_MODEL(model), &iter))
      {
        gtk_tree_model_get (GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);

        if (g_strcmp0(uid, accountinfo->id) == 0)
        {
          gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->username,
                                           COMBO_USER_MODEL_TOKEN_COL, accountinfo->token,
                                           COMBO_USER_MODEL_REFRESH_TOKEN_COL, accountinfo->refresh_token,
                                           -1);
          updated = TRUE;
          break;
        }
      }

      if (!updated)
      {
        gtk_list_store_append(model, &iter);
        gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->username,
                                         COMBO_USER_MODEL_TOKEN_COL, accountinfo->token,
                                         COMBO_USER_MODEL_REFRESH_TOKEN_COL, accountinfo->refresh_token,
                                         COMBO_USER_MODEL_ID_COL, accountinfo->id, -1);
      }
      printf("Authenticate\n");
      g_signal_handlers_block_matched (ui->comboBox_username, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, ui_combo_username_changed, NULL);
      gtk_combo_box_set_active_iter(ui->comboBox_username, &iter);
      g_signal_handlers_unblock_matched (ui->comboBox_username, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, ui_combo_username_changed, NULL);

      picasa_account_info_destroy(accountinfo);
    }
    return TRUE;
  }
}


static void ui_login_clicked(GtkButton *button, gpointer data)
{
  dt_storage_picasa_gui_data_t *ui=(dt_storage_picasa_gui_data_t*)data;
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
  if (ui->connected == FALSE)
  {
    if (ui_authenticate(ui))
    {
      ui_refresh_albums(ui);
      ui->connected = TRUE;
      gtk_button_set_label(ui->button_login, _("logout"));
    }
    else
    {
      gtk_button_set_label(ui->button_login, _("login"));
    }
  }
  else //disconnect user
  {
    if (ui->connected == TRUE && ui->picasa_api->token != NULL)
    {
      GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_username);
      GtkTreeIter iter;
      gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
      gchar *userid;
      gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);
      remove_account_info(userid);
      gtk_button_set_label(ui->button_login, _("login"));
      ui_refresh_users(ui);
      ui->connected = FALSE;
    }
  }
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), TRUE);
}



////////////////////////// darktable library interface

/* plugin name */
const char *name()
{
  return _("picasa2 webalbum");
}

/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self)
{
  self->gui_data = g_malloc0(sizeof(dt_storage_picasa_gui_data_t));
  dt_storage_picasa_gui_data_t *ui = self->gui_data;
  ui->picasa_api = picasa_api_init();

  self->widget = gtk_vbox_new(FALSE, 0);

  //create labels
  ui->label_username = GTK_LABEL(gtk_label_new(_("user")));
  ui->label_album = GTK_LABEL(gtk_label_new(_("album")));
  ui->label_album_title = GTK_LABEL(  gtk_label_new( _("title") ) );
  ui->label_album_summary = GTK_LABEL(  gtk_label_new( _("summary") ) );
  ui->label_album_privacy = GTK_LABEL(gtk_label_new(_("privacy")));
  ui->label_status = GTK_LABEL(gtk_label_new(NULL));

  gtk_misc_set_alignment(GTK_MISC(ui->label_username), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album_title), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album_summary), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album_privacy), 0.0, 0.5);

  //create entries
  GtkListStore *model_username  = gtk_list_store_new (COMBO_USER_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING); //text, token, refresh_token, id
  ui->comboBox_username = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_username)));
  GtkCellRenderer *p_cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (ui->comboBox_username), p_cell, FALSE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (ui->comboBox_username), p_cell, "text", 0, NULL);

  ui->entry_album_title = GTK_ENTRY(gtk_entry_new());
  ui->entry_album_summary = GTK_ENTRY(gtk_entry_new());

  dt_gui_key_accel_block_on_focus(GTK_WIDGET(ui->comboBox_username));
  dt_gui_key_accel_block_on_focus(GTK_WIDGET(ui->entry_album_title));
  dt_gui_key_accel_block_on_focus(GTK_WIDGET(ui->entry_album_summary));

  //retreive saved accounts
  ui_refresh_users(ui);

  //////// album list /////////
  GtkWidget *albumlist = gtk_hbox_new(FALSE, 0);
  GtkListStore *model_album = gtk_list_store_new (COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING); //name, id
  ui->comboBox_album = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_album)));
  p_cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT (ui->comboBox_album), p_cell, FALSE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (ui->comboBox_album), p_cell, "text", 0, NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
  gtk_combo_box_set_row_separator_func(ui->comboBox_album,combobox_separator,ui->comboBox_album,NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->comboBox_album), TRUE, TRUE, 0);

  ui->comboBox_privacy= GTK_COMBO_BOX(gtk_combo_box_new_text());
  GtkListStore *list_store = gtk_list_store_new (COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_INT);
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("private"), COMBO_PRIVACY_MODEL_VAL_COL, PICASA_ALBUM_PRIVACY_PRIVATE, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("public"), COMBO_PRIVACY_MODEL_VAL_COL, PICASA_ALBUM_PRIVACY_PRIVATE, -1);

  gtk_combo_box_set_model(ui->comboBox_privacy, GTK_TREE_MODEL(list_store));

  gtk_combo_box_set_active(GTK_COMBO_BOX(ui->comboBox_privacy), 1); // Set default permission to private
  ui->button_login = GTK_BUTTON(gtk_button_new_with_label(_("login")));
  ui->connected = FALSE;

  //pack the ui
  ////the auth box
  GtkWidget *hbox_auth = gtk_hbox_new(FALSE,5);
  GtkWidget *vbox_auth_labels=gtk_vbox_new(FALSE,0);
  GtkWidget *vbox_auth_fields=gtk_vbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(hbox_auth), vbox_auth_labels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_auth), vbox_auth_fields, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox_auth), TRUE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(ui->label_username), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->comboBox_username), TRUE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(gtk_label_new("")), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->button_login), TRUE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(ui->label_album), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(albumlist), TRUE, FALSE, 2);

  ////the album creation box
  ui->hbox_album = GTK_BOX(gtk_hbox_new(FALSE,5));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), TRUE); //hide it by default
  GtkWidget *vbox_album_labels=gtk_vbox_new(FALSE,0);
  GtkWidget *vbox_album_fields=gtk_vbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->hbox_album), TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(ui->hbox_album), vbox_album_labels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(ui->hbox_album), vbox_album_fields, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_title), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->entry_album_title), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_summary), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->entry_album_summary), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_privacy), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->comboBox_privacy), TRUE, FALSE, 0);

  //connect buttons to signals
  g_signal_connect(G_OBJECT(ui->button_login), "clicked", G_CALLBACK(ui_login_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_username), "changed", G_CALLBACK(ui_combo_username_changed), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_album), "changed", G_CALLBACK(ui_combo_album_changed), (gpointer)ui);

  g_object_unref(model_username);
  g_object_unref(model_album);
  g_object_unref(list_store);
}

/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self)
{
  dt_storage_picasa_gui_data_t *ui = self->gui_data;
  if (ui->picasa_api != NULL)
    picasa_api_destroy(ui->picasa_api);
  g_free(self->gui_data);
}

/* reset options to defaults */
void gui_reset(struct dt_imageio_module_storage_t *self)
{
  //TODO?
}

/* try and see if this format is supported? */
int supported(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  if( strcmp(format->mime(NULL) ,"image/jpeg") ==  0 ) return 1;
  return 0;
}

/* this actually does the work */
int store(struct dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total, const gboolean high_quality)
{
  gint result = 1;
  PicasaContext *ctx = (PicasaContext*)sdata;

  const char *ext = format->extension(fdata);
  char fname[4096]= {0};
  dt_loc_get_tmp_dir(fname,4096);
  g_strlcat (fname,"/darktable.XXXXXX.",4096);
  g_strlcat(fname,ext,4096);

  gint fd=g_mkstemp(fname);
  if(fd==-1)
  {
    dt_control_log("failed to create temporary image for picasa export");
    return 1;
  }
  close(fd);

  //get metadata
  const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, imgid);
  char *caption = NULL;
  char *description = NULL;
  GList *title = NULL;
  GList *desc = NULL;

  title = dt_metadata_get(img->id, "Xmp.dc.title", NULL);
  if(title != NULL)
  {
    caption = title->data;
  }
  if (caption == NULL)
  {
    caption = g_path_get_basename( img->filename );
    (g_strrstr(caption,"."))[0]='\0'; // Chop extension...
  }
    
  desc = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
  if (desc != NULL)
  {
    description = desc->data;
  }
     
  dt_image_cache_read_release(darktable.image_cache, img);
#if 0
  //facebook doesn't allow picture bigger than 960x960 px
  if (fdata->max_height == 0 || fdata->max_height > GOOGLE_IMAGE_MAX_SIZE)
    fdata->max_height = GOOGLE_IMAGE_MAX_SIZE;
  if (fdata->max_width == 0 || fdata->max_width > GOOGLE_IMAGE_MAX_SIZE)
    fdata->max_width = GOOGLE_IMAGE_MAX_SIZE;
#endif

  if(dt_imageio_export(imgid, fname, format, fdata, high_quality) != 0)
  {
    g_printerr("[picasa] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 0;
    goto cleanup;
  }

  if (ctx->album_id == NULL)
  {
    if (ctx->album_title == NULL)
    {
      dt_control_log(_("unable to create album, no title provided"));
      result = 0;
      goto cleanup;
    }
    const gchar *album_id = picasa_create_album(ctx, ctx->album_title, ctx->album_summary, ctx->album_permission);
    if (album_id == NULL)
    {
      dt_control_log(_("unable to create album"));
      result = 0;
      goto cleanup;
    }
    g_snprintf (ctx->album_id, 1024, "%s", album_id);
  }

  const char *photoid = picasa_upload_photo_to_album(ctx, ctx->album_id, fname, caption, description, imgid);
  if (photoid == NULL)
  {
    dt_control_log(_("unable to export photo to webalbum"));
    result = 0;
    goto cleanup;
  }

cleanup:
  unlink( fname );
  g_free( caption );
  g_free( description );
  if(desc)
  {
    //no need to free desc->data as caption points to it
    g_list_free(desc);
  }

  if (result)
  {
    //this makes sense only if the export was successful
    dt_control_log(_("%d/%d exported to picasa webalbum"), num, total );
  }
  return 0;
}


int finalize_store(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  gdk_threads_enter();
  dt_storage_picasa_gui_data_t *ui = (dt_storage_picasa_gui_data_t*)self->gui_data;
  ui_reset_albums_creation(ui);
  ui_refresh_albums(ui);
  gdk_threads_leave();
  return 1;
}


void *get_params(struct dt_imageio_module_storage_t *self, int *size)
{
  dt_storage_picasa_gui_data_t *ui = (dt_storage_picasa_gui_data_t*)self->gui_data;
  if(ui->picasa_api == NULL || ui->picasa_api->token == NULL)
  {
    return NULL;
  }
  PicasaContext *p = (PicasaContext*)g_malloc0(sizeof(PicasaContext));
  *size = sizeof(PicasaContext) - 8*sizeof(void *);

  p->curl_ctx = ui->picasa_api->curl_ctx;
  p->json_parser = ui->picasa_api->json_parser;
  p->errmsg = ui->picasa_api->errmsg;
  p->token = g_strdup(ui->picasa_api->token);
  p->refresh_token = g_strdup(ui->picasa_api->refresh_token);

  int index = gtk_combo_box_get_active(ui->comboBox_album);
  if (index < 0)
  {
    picasa_api_destroy(p);
    return NULL;
  }
  else if (index == 0)
  {
    p->album_id[0] = 0;
    p->album_title = g_strdup(gtk_entry_get_text(ui->entry_album_title));
    p->album_summary = g_strdup(gtk_entry_get_text(ui->entry_album_summary));
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_privacy);
    GtkTreeIter iter;
    int permission = -1;
    gtk_combo_box_get_active_iter(ui->comboBox_privacy, &iter);
    gtk_tree_model_get(model, &iter, COMBO_PRIVACY_MODEL_VAL_COL, &permission, -1);
    p->album_permission = permission;
  }
  else
  {
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_album);
    GtkTreeIter iter;
    gchar *albumid = NULL;
    gtk_combo_box_get_active_iter(ui->comboBox_album, &iter);
    gtk_tree_model_get(model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    g_snprintf(p->album_id, 1024, "%s", albumid);
  }

  g_snprintf(p->userid, 1024, "%s", ui->picasa_api->userid);

  //recreate a new context for further usages
  ui->picasa_api = picasa_api_init();
  ui->picasa_api->token = g_strdup(p->token);
  ui->picasa_api->refresh_token = g_strdup(p->refresh_token);
  g_snprintf(ui->picasa_api->userid, 1024, "%s", p->userid);

  return p;
}


void free_params(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  PicasaContext *ctx = (PicasaContext*)data;
  picasa_api_destroy(ctx);
}

int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != sizeof(PicasaContext) - 8*sizeof(void *))
    return 1;

  PicasaContext *d = (PicasaContext *) params;
  dt_storage_picasa_gui_data_t *g = (dt_storage_picasa_gui_data_t *)self->gui_data;

  g_snprintf(g->picasa_api->album_id, 1024, "%s", d->album_id);
  g_snprintf(g->picasa_api->userid, 1024, "%s", d->userid);

  GtkListStore *model =  GTK_LIST_STORE(gtk_combo_box_get_model(g->comboBox_username));
  GtkTreeIter iter;
  gboolean r;
  gchar *uid = NULL;
  gchar *albumid = NULL;

  for (r = gtk_tree_model_get_iter_first (GTK_TREE_MODEL(model), &iter);
       r == TRUE;
       r = gtk_tree_model_iter_next (GTK_TREE_MODEL(model), &iter))
  {
    gtk_tree_model_get (GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);
    if (g_strcmp0(uid, g->picasa_api->userid) == 0)
    {
      gtk_combo_box_set_active_iter(g->comboBox_username, &iter);
      break;
    }
  }
  g_free(uid);

  model =  GTK_LIST_STORE(gtk_combo_box_get_model(g->comboBox_album));
  for (r = gtk_tree_model_get_iter_first (GTK_TREE_MODEL(model), &iter);
       r == TRUE;
       r = gtk_tree_model_iter_next (GTK_TREE_MODEL(model), &iter))
  {
    gtk_tree_model_get (GTK_TREE_MODEL(model), &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    if (g_strcmp0(albumid, g->picasa_api->album_id) == 0)
    {
      gtk_combo_box_set_active_iter(g->comboBox_album, &iter);
      break;
    }
  }
  g_free(albumid);
  
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;