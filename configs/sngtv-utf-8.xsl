<?xml version="1.0" encoding="UTF-8"?>
<!--
   sngtv.xsl - transforms the intermediate xml created by the sngtv plugin
   into the epgd event format. Install as sngtv-utf-8.xsl and sngtv-iso-8859-1.xsl.
-->
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:str="http://exslt.org/strings"
                extension-element-prefixes="str">

<xsl:output method="xml" encoding="UTF-8"/>

<xsl:variable name="mapping" select="document('sngtv-category.xml')/mapping"/>
<xsl:variable name="lower" select="'ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÜ'"/>
<xsl:variable name="upper2lower" select="'abcdefghijklmnopqrstuvwxyzäöü'"/>

<xsl:template match="/">
<events>
   <xsl:for-each select="sngtv/event">
   <event>
      <xsl:attribute name="id"><xsl:value-of select="@id"/></xsl:attribute>

      <starttime><xsl:value-of select="starttime"/></starttime>
      <duration><xsl:value-of select="duration"/></duration>
      <title><xsl:value-of select='translate(title, "–’", "-&apos;")'/></title>

      <xsl:if test="string-length(subtitle)"><shorttext><xsl:value-of select="subtitle"/></shorttext></xsl:if>
      <xsl:if test="string-length(description)"><longdescription><xsl:value-of select="description"/></longdescription></xsl:if>

      <xsl:variable name="genre1">
         <xsl:choose>
            <xsl:when test="contains(genres, ',')"><xsl:value-of select="normalize-space(substring-before(genres, ','))"/></xsl:when>
            <xsl:otherwise><xsl:value-of select="normalize-space(genres)"/></xsl:otherwise>
         </xsl:choose>
      </xsl:variable>
      <xsl:variable name="genresLower" select="translate($genre1, $lower, $upper2lower)"/>
      <xsl:variable name="mapped" select="($mapping/categories/category[contains/text()[contains($genresLower, .)]])[1]/@name"/>

      <xsl:if test="string-length($genre1)"><genre><xsl:value-of select="$genre1"/></genre></xsl:if>

      <category>
         <xsl:choose>
            <xsl:when test="$mapped = 'Spielfilm' and (string-length(season) or string-length(episode) or duration &lt; 4500)">Serie</xsl:when>
            <xsl:when test="string-length($mapped)"><xsl:value-of select="$mapped"/></xsl:when>
            <xsl:when test="string-length(season) or string-length(episode)">Serie</xsl:when>
            <xsl:otherwise>Sonstige</xsl:otherwise>
         </xsl:choose>
      </category>

      <xsl:if test="string-length(country)"><country><xsl:value-of select="country"/></country></xsl:if>
      <xsl:if test="string-length(year)"><year><xsl:value-of select="year"/></year></xsl:if>
      <xsl:if test="string-length(actor)"><actor><xsl:value-of select="str:replace(actor, ',', ', ')"/></actor></xsl:if>
      <xsl:if test="string-length(director)"><director><xsl:value-of select="str:replace(director, ',', ', ')"/></director></xsl:if>
      <xsl:if test="string-length(producer)"><producer><xsl:value-of select="str:replace(producer, ',', ', ')"/></producer></xsl:if>

      <xsl:if test="number(episode) &gt; 0"><extepnum><xsl:value-of select="episode"/></extepnum></xsl:if>
      <xsl:if test="string-length(season) or string-length(episode)">
         <other>
            <xsl:if test="string-length(season)">Staffel <xsl:value-of select="season"/></xsl:if>
            <xsl:if test="string-length(season) and string-length(episode)">, </xsl:if>
            <xsl:if test="string-length(episode)">Folge <xsl:value-of select="episode"/></xsl:if>
         </other>
      </xsl:if>

      <xsl:if test="string-length(image)">
         <images><xsl:value-of select="image"/></images>
         <imagetype>jpg</imagetype>
      </xsl:if>
   </event>
   </xsl:for-each>
</events>
</xsl:template>

</xsl:stylesheet>
