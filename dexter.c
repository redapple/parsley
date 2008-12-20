#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <json/json.h>
#include "kstring.h"
#include "obstack.h"
#include "dexter.h"
#include "y.tab.h"
#include "printbuf.h"
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxml/parser.h>
#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/xmlwriter.h>
#include <libexslt/exslt.h>

int yywrap(void){
  return 1;
}

xmlDocPtr dex_parse_file(dexPtr dex, char* file, boolean html) {
	if(html) {
		htmlParserCtxtPtr htmlCtxt = htmlNewParserCtxt();
  	htmlDocPtr html = htmlCtxtReadFile(htmlCtxt, file, "UTF-8", 3);
		if(html == NULL) {
			asprintf(&dex->error, "Couldn't parse file: %s\n", file);
			return NULL;
		}
		return dex_parse_doc(dex, html);
	} else {
		xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
		xmlDocPtr xml = xmlCtxtReadFile(ctxt, file, "UTF-8", 3);
		if(xml == NULL) {
			asprintf(&dex->error, "Couldn't parse file: %s\n", file);
			return NULL;
		}
		return dex_parse_doc(dex, xml);
	}
}

xmlDocPtr dex_parse_string(dexPtr dex, char* string, size_t size, boolean html) {
	if(html) {
		htmlParserCtxtPtr htmlCtxt = htmlNewParserCtxt();
  	htmlDocPtr html = htmlCtxtReadMemory(htmlCtxt, string, size, "http://foo", "UTF-8", 3);
		if(html == NULL) {
			asprintf(&dex->error, "Couldn't parse string\n");
			return NULL;
		}
		return dex_parse_doc(dex, html);
	} else {
		xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
 		xmlDocPtr xml = xmlCtxtReadMemory(ctxt, string, size, "http://foo", "UTF-8", 3);
		if(xml == NULL) {
			asprintf(&dex->error, "Couldn't parse string\n");
			return NULL;
		}
		return dex_parse_doc(dex, xml);
	}
}

xmlDocPtr dex_parse_doc(dexPtr dex, xmlDocPtr doc) {
	return xsltApplyStylesheet(dex->stylesheet, doc, NULL);
}

dexPtr dex_compile(char* dex_str, char* incl) {
	dexPtr dex = (dexPtr) calloc(sizeof(compiled_dex), 1);
	
	if(last_dex_error != NULL) {
		free(last_dex_error);
		last_dex_error = NULL;
	}
	
  if(!dex_exslt_registered) {
    exsltRegisterAll();
    dex_exslt_registered = true;
  }

	obstack_init(&dex_obstack);
	
	struct json_object *json = json_tokener_parse(dex_str);
	if(is_error(json)) {
		dex->error = strdup("Your dex is not valid json.");
		return dex;
	}

	struct printbuf* buf = printbuf_new();
	
	sprintbuf(buf, "<xsl:stylesheet version=\"1.0\" xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\"");
	sprintbuf(buf, " xmlns:dexter=\"http://kylemaxwell.com/dexter\"");
	sprintbuf(buf, " xmlns:str=\"http://exslt.org/strings\"");
	sprintbuf(buf, " xmlns:set=\"http://exslt.org/sets\"");
	sprintbuf(buf, " xmlns:math=\"http://exslt.org/math\"");
	sprintbuf(buf, " xmlns:func=\"http://exslt.org/functions\"");
	sprintbuf(buf, " xmlns:user=\"http://kylemaxwell.com/dexter/user-functions\"");
	sprintbuf(buf, " xmlns:dyn=\"http://exslt.org/dynamic\"");
	sprintbuf(buf, " xmlns:date=\"http://exslt.org/dates-and-times\"");
	sprintbuf(buf, " xmlns:exsl=\"http://exslt.org/common\"");
	sprintbuf(buf, " xmlns:saxon=\"http://icl.com/saxon\"");
	sprintbuf(buf, " exclude-result-prefixes=\"str math set func dyn exsl saxon user date\"");
	sprintbuf(buf, ">\n");
	sprintbuf(buf, "<xsl:output method=\"xml\" indent=\"yes\"/>\n");
	sprintbuf(buf, "<xsl:strip-space elements=\"*\"/>\n");
	sprintbuf(buf, "%s\n", incl);
	sprintbuf(buf, "<xsl:template match=\"/\">\n");
	sprintbuf(buf, "<dexter:root>\n");
	
	char *context = "root";
	__dex_recurse(json, buf, context);
	json_object_put(json); // frees json
	dex->error = last_dex_error;
	
	sprintbuf(buf, "</dexter:root>\n");
	sprintbuf(buf, "</xsl:template>\n");
	sprintbuf(buf, "</xsl:stylesheet>\n");
	
	if(dex->error == NULL) {
		xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
		xmlDocPtr doc = xmlCtxtReadMemory(ctxt, buf->buf, buf->size, "http://kylemaxwell.com/some-dex", "UTF-8", 3);
		dex->stylesheet = xsltParseStylesheetDoc(doc);
	}
	
	printbuf_free(buf);
	obstack_free(&dex_obstack, NULL);
	
	return dex;
}

void dex_free(dexPtr ptr) {
	if(ptr->error != NULL) free(ptr->error);
	if(ptr->stylesheet != NULL) xsltFreeStylesheet(ptr->stylesheet);
	free(ptr);
}

void * __dex_alloc(int size) {
	return obstack_alloc(&dex_obstack, size);
}

void __dex_recurse_object(struct json_object * json, struct printbuf* buf, char *context) {
	json_object_object_foreach(json, key, val) {
		__dex_recurse_foreach(json, key, val, buf, context);
  }
}

void __dex_recurse_foreach(struct json_object * json, char* key, struct json_object * val, struct printbuf* buf, char *context) {
	char *tag = astrdup(key);;
	char *ptr = tag;
	char *expr = astrdup(key);;
	int offset = 0;
	bool has_expr = false;
	struct json_object * inner;
	
	while(*ptr++ != '\0'){
		offset++;
		if(*ptr == '(') {
			*ptr = 0;
			has_expr = true;
			break;
		}
	}
	expr += offset;
	
	sprintbuf(buf, "<%s>\n", tag);	
	switch(json_object_get_type(val)) {
		case json_type_array:
			printf("arr");
			inner = json_object_array_get_idx(val, 0);
			switch(json_object_get_type(inner)) {
				case json_type_string:
					if(has_expr) {
						sprintbuf(buf, "<dexter:groups><xsl:for-each select=\"%s\"><dexter:group>\n", myparse(expr));
						__dex_recurse(inner, buf, astrcat3(context, ".", tag));
					} else {
						sprintbuf(buf, "<dexter:groups><xsl:for-each select=\"%s\"><dexter:group>\n", myparse(json_object_get_string(inner)));
						struct json_object * dot = json_object_new_string(".");
						__dex_recurse(dot, buf, astrcat3(context, ".", tag));						
					}
					sprintbuf(buf, "</dexter:group></xsl:for-each></dexter:groups>\n");
					break;
				case json_type_object:
					break;
			}
			break;
		case json_type_string:
			printf("str");
			break;
		case json_type_object:
			printf("obj");
			break;
	};
	sprintbuf(buf, "</%s>\n", tag);	
	
	
	// char *tag;
	// char *ptr;
	// char *expr;
	// int offset;
	// bool has_expr;
	// int is_arr;
	// int is_simple_arr;
	// offset = 0;
	// tag = astrdup(key);
	// expr = astrdup(key);
	// ptr = tag;
	// has_expr = false;
	// while(*ptr++ != '\0'){
	// 	offset++;
	// 	if(*ptr == '(') {
	// 		*ptr = 0;
	// 		has_expr = true;
	// 		break;
	// 	}
	// }
	// expr += offset;
	// 
	// if(!has_expr) expr = ".";
	// if(is_arr = json_object_is_type(val, json_type_array) ) {
	// 	struct json_object * inner = json_object_array_get_idx(val, 0);
	// 	if(is_simple_arr = json_object_is_type(inner, json_type_string)) {
	// 		expr = json_object_get_string(inner);
	// 	}
	// }
	// 
	// sprintbuf(buf, "<%s>\n", tag);
	// if(is_arr) sprintbuf(buf, "<dexter:groups><xsl:for-each select=\"%s\">\n", myparse(expr));
	// __dex_recurse(val, buf, astrcat3(context, ".", tag));
	// if(is_arr) sprintbuf(buf, "</xsl:for-each></dexter:groups>\n");
	// sprintbuf(buf, "</%s>\n", tag);	
}


void __dex_recurse_array(struct json_object * json, struct printbuf* buf, char *context) {
	printf("WTF!!!!!!!\n");
}

void __dex_recurse_string(struct json_object * json, struct printbuf* buf, char *context) {
	char* a = astrdup(json_object_get_string(json));
	char* ptr = context;
	char* last = context;
	char* expr;
	while(*ptr++){
		if(*ptr == '.') last = ptr + 1;
	}
	dex_parsing_context = context;
	expr = myparse(a);
	sprintbuf(buf, "<xsl:variable name=\"%s\" select=\"%s\" />\n", context, expr);
	sprintbuf(buf, "<xsl:variable name=\"%s\" select=\"$%s\" />\n", last, context);
	sprintbuf(buf, "<xsl:value-of select=\"$%s\" />\n", context);
}

void yyerror(const char * s) {
	struct printbuf *buf = printbuf_new();
	if(last_dex_error !=NULL) sprintbuf(buf, "%s\n", last_dex_error);
  sprintbuf(buf, "%s in key: %s", s, dex_parsing_context);
	last_dex_error = strdup(buf->buf);
	printbuf_free(buf);
}

void __dex_recurse(struct json_object * json, struct printbuf* buf, char *context) {
	switch(json_object_get_type(json)){
		case json_type_object:
			__dex_recurse_object(json, buf, context);
			break;
		case json_type_array:
			__dex_recurse_array(json, buf, context);
			break;
		case json_type_string:
			__dex_recurse_string(json, buf, context);
			break;
		case json_type_boolean:
		case json_type_double:
		case json_type_int:
		default:
			fprintf(stderr, "Invalid type in json\n");
			exit(1);
	}
}
